/* ************************************************************************
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 * ************************************************************************ */
#include "hipblaslt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <hip/hip_runtime.h>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifndef CHECK_HIP_ERROR
#define CHECK_HIP_ERROR(error)                                                 \
  if (error != hipSuccess) {                                                   \
    fprintf(stderr, "Hip error: '%s'(%d) at %s:%d\n",                          \
            hipGetErrorString(error), error, __FILE__, __LINE__);              \
    exit(EXIT_FAILURE);                                                        \
  }
#endif

#ifndef CHECK_HIPBLASLT_ERROR
#define CHECK_HIPBLASLT_ERROR(error)                                           \
  if (error != HIPBLASLT_STATUS_SUCCESS) {                                     \
    fprintf(stderr, "hipBLASLt error(Err=%d) at %s:%d\n", error, __FILE__,     \
            __LINE__);                                                         \
    fprintf(stderr, "\n");                                                     \
    exit(EXIT_FAILURE);                                                        \
  }
#endif

// default sizes
#define DIM1 1024
#define DIM2 1024
#define DIM3 1024
#define BATCH_COUNT 1
#define ALPHA 2
#define BETA 3
#define BENCH_LOOP_COUNT 3

typedef enum _ActivationType {
  NONE = 0,
  RELU = 1,
  GELU = 2,
} ActivationType;

inline const char *ToString(ActivationType act) {
  switch (act) {
  case NONE:
    return "none";
  case RELU:
    return "relu";
  case GELU:
    return "gelu";
  default:
    return "[Unknown Activation Type]";
  }
}

auto _relu = [](auto in) -> decltype(in) {
  return static_cast<decltype(in)>(std::max(static_cast<decltype(in)>(0), in));
};

auto _gelu = [](auto in) -> decltype(in) {
  using Tc = float;

  constexpr auto k0 = static_cast<Tc>(0.7978845608028654);
  constexpr auto k1 = static_cast<Tc>(0.044715);
  Tc in_Tc = static_cast<Tc>(in);

  return static_cast<decltype(in)>(
      0.5f *
      (in_Tc * (1.f + std::tanh(k0 * (in_Tc * (1.f + k1 * (in_Tc * in_Tc)))))));
};

template <typename T>
void activation(int64_t m, int64_t n, int64_t ld, T *addr,
                ActivationType actType) {
  auto saturate = [](T val) { return static_cast<T>(val); };
  std::function<T(T)> actFunc;
  if (actType == ActivationType::RELU)
    actFunc = _relu;
  else if (actType == ActivationType::GELU)
    actFunc = _gelu;

  for (int i = 0; i < m; i++) {
#pragma omp parallel for
    for (int j = 0; j < n; j++) {
      auto pos = j * ld + i;
      auto data = static_cast<T>(*(addr + pos));
      *(addr + pos) = saturate(actFunc(data));
    }
  }
}

template <typename T> inline bool AlmostEqual(T a, T b) {
  T absA = (a > 0) ? a : -a;
  T absB = (b > 0) ? b : -b;
  // this avoids NaN when inf is compared against inf in the alternative code
  // path
  if (static_cast<float>(absA) ==
          std::numeric_limits<float>::infinity() || // numeric_limits is yet to
      // support _Float16 type
      // properly;
      static_cast<float>(absB) ==
          std::numeric_limits<float>::infinity()) // however promoting it to
  // float works just as fine
  {
    return a == b;
  }
  T absDiff = (a - b > 0) ? a - b : b - a;
  return absDiff / (absA + absB + 1) < 0.001;
}

template <typename T>
void print_strided_batched(const char *name, T *A, int64_t n1, int64_t n2,
                           int64_t n3, int64_t s1, int64_t s2, int64_t s3) {
  // n1, n2, n3 are matrix dimensions, sometimes called m, n, batch_count
  // s1, s1, s3 are matrix strides, sometimes called 1, lda, stride_a
  printf("---------- %s (MxN=%ldx%ld,batch=%ld,stride0=%ld, "
         "stride1=%ld)----------\n",
         name, n1, n2, n3, s1, s2);
  int max_size = 128;

  for (int i3 = 0; i3 < n3 && i3 < max_size; i3++) {
    for (int i1 = 0; i1 < n1 && i1 < max_size; i1++) {
      for (int i2 = 0; i2 < n2 && i2 < max_size; i2++) {
        printf("[%ld]\t%8.3f\t", (i1 * s1) + (i2 * s2) + (i3 * s3),
               static_cast<float>(A[(i1 * s1) + (i2 * s2) + (i3 * s3)]));
      }
      printf("\n");
    }
    if (i3 < (n3 - 1) && i3 < (max_size - 1))
      printf("\n");
  }
}

template <typename Ti, typename To, typename Tc>
void mat_mat_mult(Tc alpha, Tc beta, int M, int N, int K, int batch_count,
                  const Ti *A, int As1, int As2, int As3, const Ti *B, int Bs1,
                  int Bs2, int Bs3, const To *C, int Cs1, int Cs2, int Cs3,
                  To *D, int Ds1, int Ds2, int Ds3, To *bias) {
  for (int batch = 0; batch < batch_count; batch++) {
    for (int i1 = 0; i1 < M; i1++) {
      for (int i2 = 0; i2 < N; i2++) {
        Tc t = static_cast<Tc>(0);
        for (int i3 = 0; i3 < K; i3++) {
          t += static_cast<Tc>(A[i1 * As1 + i3 * As2 + batch * As3]) *
               static_cast<Tc>(B[i3 * Bs1 + i2 * Bs2 + batch * Bs3]);
        }
        D[i1 * Ds1 + i2 * Ds2 + batch * Ds3] =
            static_cast<To>(
                beta * static_cast<Tc>(C[i1 * Cs1 + i2 * Cs2 + batch * Cs3]) +
                alpha * t) +
            (bias == nullptr ? 0 : bias[i1]);
      }
    }
  }
}

// cppcheck-suppress constParameter
static void show_usage(char *argv[]) {
  std::cerr << "Usage: " << argv[0] << " <options>\n"
            << "options:\n"
            << "\t-h, --help\t\t\t\tShow this help message\n"
            << "\t-v, --verbose\t\t\t\tVerbose output\n"
            << "\t-V, --validate\t\t\t\tVerify results\n"
            << "\t-m \t\t\tm\t\tGEMM_STRIDED argument m\n"
            << "\t-n \t\t\tn\t\tGEMM_STRIDED argument n\n"
            << "\t-k \t\t\tk \t\tGEMM_STRIDED argument k\n"
            << "\t--lda \t\t\tlda \t\tGEMM_STRIDED argument lda\n"
            << "\t--ldb \t\t\tldb \t\tGEMM_STRIDED argument ldb\n"
            << "\t--ldc \t\t\tldc \t\tGEMM_STRIDED argument ldc\n"
            << "\t--ldd \t\t\tldd \t\tGEMM_STRIDED argument ldd\n"
            << "\t--trans_a \t\ttrans_a \tGEMM_STRIDED argument trans_a\n"
            << "\t--trans_b \t\ttrans_b \tGEMM_STRIDED argument trans_b\n"
            << "\t--datatype \t\tdatatype \tGEMM_STRIDED argument in out "
               "datatype:fp32\n"
            << "\t--stride_a \t\tstride_a \tGEMM_STRIDED argument stride_a\n"
            << "\t--stride_b \t\tstride_b \tGEMM_STRIDED argument stride_b\n"
            << "\t--stride_c \t\tstride_c \tGEMM_STRIDED argument stride_c\n"
            << "\t--stride_d \t\tstride_d \tGEMM_STRIDED argument stride_d\n"
            << "\t--alpha \t\talpha \t\tGEMM_STRIDED argument alpha\n"
            << "\t--beta \t\t\tbeta \t\tGEMM_STRIDED argument beta\n"
            << "\t--act \t\t\tact \t\tGEMM_STRIDED set activation type: relu "
               "or gelu\n"
            << "\t--bias \t\t\tbias \t\tGEMM_STRIDED enable bias: 0 or 1 "
               "(default is 0)\n"
            << "\t--header \t\theader \t\tPrint header for output (default is "
               "enabled)\n"
            << "\t--timing \t\ttiming \t\tBechmark GPU kernel performance:0 or "
               "1 (default is 1)\n"
            << std::endl;
}

static int parse_arguments(int argc, char *argv[], hipDataType &in_out_datatype,
                           int64_t &m, int64_t &n, int64_t &k, int64_t &lda,
                           int64_t &ldb, int64_t &ldc, int64_t &ldd,
                           int64_t &stride_a, int64_t &stride_b,
                           int64_t &stride_c, int64_t &stride_d,
                           int &batch_count, float &alpha, float &beta,
                           hipblasLtOperation_t &trans_a,
                           hipblasLtOperation_t &trans_b, bool &enable_bias,
                           ActivationType &actType, bool &header, bool &verbose,
                           bool &validate, bool &timing) {
  if (argc >= 2) {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if ((arg.at(0) == '-') || ((arg.at(0) == '-') && (arg.at(1) == '-'))) {
        if ((arg == "-h") || (arg == "--help")) {
          return EXIT_FAILURE;
        }
        if ((arg == "-v") || (arg == "--verbose")) {
          verbose = true;
        } else if ((arg == "-V") || (arg == "--validate")) {
          validate = true;
        } else if (arg == "--header") {
          header = true;
        } else if (arg == "--timing") {
          timing = atoi(argv[++i]);
        } else if ((arg == "-m") && (i + 1 < argc)) {
          m = atoi(argv[++i]);
        } else if ((arg == "-n") && (i + 1 < argc)) {
          n = atoi(argv[++i]);
        } else if ((arg == "-k") && (i + 1 < argc)) {
          k = atoi(argv[++i]);
        } else if ((arg == "--batch_count") && (i + 1 < argc)) {
          batch_count = atoi(argv[++i]);
        } else if ((arg == "--lda") && (i + 1 < argc)) {
          lda = atoi(argv[++i]);
        } else if ((arg == "--ldb") && (i + 1 < argc)) {
          ldb = atoi(argv[++i]);
        } else if ((arg == "--ldc") && (i + 1 < argc)) {
          ldc = atoi(argv[++i]);
        } else if ((arg == "--ldd") && (i + 1 < argc)) {
          ldd = atoi(argv[++i]);
        } else if ((arg == "--stride_a") && (i + 1 < argc)) {
          stride_a = atoi(argv[++i]);
        } else if ((arg == "--stride_b") && (i + 1 < argc)) {
          stride_b = atoi(argv[++i]);
        } else if ((arg == "--stride_c") && (i + 1 < argc)) {
          stride_c = atoi(argv[++i]);
        } else if ((arg == "--stride_d") && (i + 1 < argc)) {
          stride_d = atoi(argv[++i]);
        } else if ((arg == "--alpha") && (i + 1 < argc)) {
          alpha = atof(argv[++i]);
        } else if ((arg == "--beta") && (i + 1 < argc)) {
          beta = atof(argv[++i]);
        } else if ((arg == "--bias") && (i + 1 < argc)) {
          enable_bias = atoi(argv[++i]);
        } else if ((arg == "--act") && (i + 1 < argc)) {
          ++i;
          if (strncmp(argv[i], "relu", 4) == 0)
            actType = ActivationType::RELU;
          else if (strncmp(argv[i], "gelu", 4) == 0)
            actType = ActivationType::GELU;
          else {
            std::cerr << "error with " << arg << std::endl;
            std::cerr << "do not recognize value " << argv[i];
            return EXIT_FAILURE;
          }
        } else if ((arg == "--trans_a") && (i + 1 < argc)) {
          ++i;
          if (strncmp(argv[i], "N", 1) == 0 || strncmp(argv[i], "n", 1) == 0) {
            trans_a = HIPBLASLT_OP_N;
          } else if (strncmp(argv[i], "T", 1) == 0 ||
                     strncmp(argv[i], "t", 1) == 0) {
            trans_a = HIPBLASLT_OP_T;
          } else {
            std::cerr << "error with " << arg << std::endl;
            std::cerr << "do not recognize value " << argv[i];
            return EXIT_FAILURE;
          }
        } else if ((arg == "--trans_b") && (i + 1 < argc)) {
          ++i;
          if (strncmp(argv[i], "N", 1) == 0 || strncmp(argv[i], "n", 1) == 0) {
            trans_b = HIPBLASLT_OP_N;
          } else if (strncmp(argv[i], "T", 1) == 0 ||
                     strncmp(argv[i], "t", 1) == 0) {
            trans_b = HIPBLASLT_OP_T;
          } else {
            std::cerr << "error with " << arg << std::endl;
            std::cerr << "do not recognize value " << argv[i];
            return EXIT_FAILURE;
          }
        } else if ((arg == "--datatype") && (i + 1 < argc)) {
          ++i;
          if (strncmp(argv[i], "fp32", 4) == 0) {
            in_out_datatype = HIP_R_32F;
          } else {
            std::cerr << "error with " << arg << std::endl;
            std::cerr << "do not recognize value " << argv[i];
            return EXIT_FAILURE;
          }
        } else {
          std::cerr << "error with " << arg << std::endl;
          std::cerr << "do not recognize option" << std::endl << std::endl;
          return EXIT_FAILURE;
        }
      } else {
        std::cerr << "error with " << arg << std::endl;
        std::cerr << "option must start with - or --" << std::endl << std::endl;
        return EXIT_FAILURE;
      }
    }
  }
  return EXIT_SUCCESS;
}

bool bad_argument(hipblasLtOperation_t trans_a, hipblasLtOperation_t trans_b,
                  int64_t m, int64_t n, int64_t k, int64_t lda, int64_t ldb,
                  int64_t ldc, int64_t ldd, int64_t stride_a, int64_t stride_b,
                  int64_t stride_c, int64_t stride_d, int64_t batch_count) {
  bool argument_error = false;
  if ((trans_a == HIPBLASLT_OP_N) && (lda < m)) {
    argument_error = true;
    std::cerr << "ERROR: bad argument lda = " << lda << " < " << m << std::endl;
  }
  if ((trans_a == HIPBLASLT_OP_T) && (lda < k)) {
    argument_error = true;
    std::cerr << "ERROR: bad argument lda = " << lda << " < " << k << std::endl;
  }
  if ((trans_b == HIPBLASLT_OP_N) && (ldb < k)) {
    argument_error = true;
    std::cerr << "ERROR: bad argument ldb = " << ldb << " < " << k << std::endl;
  }
  if ((trans_b == HIPBLASLT_OP_T) && (ldb < n)) {
    argument_error = true;
    std::cerr << "ERROR: bad argument ldb = " << ldb << " < " << n << std::endl;
  }
  if (stride_a < 0) {
    argument_error = true;
    std::cerr << "ERROR: bad argument stride_a < 0" << std::endl;
  }
  if (stride_b < 0) {
    argument_error = true;
    std::cerr << "ERROR: bad argument stride_b < 0" << std::endl;
  }
  if (ldc < m) {
    argument_error = true;
    std::cerr << "ERROR: bad argument ldc = " << ldc << " < " << m << std::endl;
  }
  if (stride_c < n * ldc) {
    argument_error = true;
    std::cerr << "ERROR: bad argument stride_c = " << stride_c << " < "
              << n * ldc << std::endl;
  }
  if (ldd < m) {
    argument_error = true;
    std::cerr << "ERROR: bad argument ldc = " << ldd << " < " << m << std::endl;
  }
  if (stride_d < n * ldd) {
    argument_error = true;
    std::cerr << "ERROR: bad argument stride_c = " << stride_d << " < "
              << n * ldd << std::endl;
  }
  if (batch_count != 1) {
    argument_error = true;
    std::cerr << "ERROR: bad argument batch_count = " << batch_count
              << std::endl;
  }

  return argument_error;
}

template <typename T>
void initialize_a_b_c_bias(std::vector<T> &ha, int64_t size_a,
                           std::vector<T> &hb, int64_t size_b,
                           std::vector<T> &hc, int64_t size_c,
                           std::vector<T> &h_bias, int64_t size_bias) {
  srand(1);
  for (int i = 0; i < size_a; ++i) {
    ha[i] = static_cast<T>((rand() % 7) - 3);
  }
  for (int i = 0; i < size_b; ++i) {
    hb[i] = static_cast<T>((rand() % 7) - 3);
  }
  for (int i = 0; i < size_c; ++i) {
    hc[i] = static_cast<T>((rand() % 7) - 3);
  }
  for (int i = 0; i < size_bias; ++i) {
    h_bias[i] = static_cast<T>((rand() % 7) - 3);
  }
}

template <typename T>
void test_hipblaslt(hipDataType in_out_datatype, hipblasLtOperation_t trans_a,
                    hipblasLtOperation_t trans_b, int64_t m, int64_t n,
                    int64_t k, int64_t lda, int64_t ldb, int64_t ldc,
                    int64_t ldd, int64_t stride_a, int64_t stride_b,
                    int64_t stride_c, int64_t stride_d, int64_t batch_count,
                    float alpha, float beta, bool enable_bias,
                    ActivationType actType, bool validate, bool verbose,
                    bool timing) {
  int64_t a_stride_1, a_stride_2, b_stride_1, b_stride_2;
  int64_t row_a, col_a, row_b, col_b, row_c, col_c;
  int size_a1, size_b1, size_c1 = ldc * n;
  if (trans_a == HIPBLASLT_OP_N) {
    std::cout << "N";
    row_a = m;
    col_a = k;
    a_stride_1 = 1;
    a_stride_2 = lda;
    size_a1 = lda * k;
  } else {
    std::cout << "T";
    row_a = k;
    col_a = m;
    a_stride_1 = lda;
    a_stride_2 = 1;
    size_a1 = lda * m;
  }
  if (trans_b == HIPBLASLT_OP_N) {
    std::cout << "N, ";
    row_b = k;
    col_b = n;
    b_stride_1 = 1;
    b_stride_2 = ldb;
    size_b1 = ldb * n;
  } else {
    std::cout << "T, ";
    row_b = n;
    col_b = k;
    b_stride_1 = ldb;
    b_stride_2 = 1;
    size_b1 = ldb * k;
  }
  row_c = m;
  col_c = n;

  int size_a =
      batch_count == 0 ? size_a1 : size_a1 + stride_a * (batch_count - 1);
  int size_b =
      batch_count == 0 ? size_b1 : size_b1 + stride_b * (batch_count - 1);
  int size_c =
      batch_count == 0 ? size_c1 : size_c1 + stride_c * (batch_count - 1);
  int size_d = size_c;
  int size_bias = enable_bias ? m : 0;

  // Naming: da is in GPU (device) memory. ha is in CPU (host) memory
  std::vector<T> ha(size_a);
  std::vector<T> hb(size_b);
  std::vector<T> hc(size_c);
  std::vector<T> hd(size_c);
  std::vector<T> hd_gold(size_d);
  std::vector<T> h_bias(size_bias);

  // initial data on host
  initialize_a_b_c_bias(ha, size_a, hb, size_b, hc, size_c, h_bias, size_bias);

  // allocate memory on device
  void *da, *db, *dc, *dd, *d_bias;
  int num_streams = 1;
  hipStream_t stream = nullptr;

  CHECK_HIP_ERROR(hipMalloc(&da, size_a * sizeof(T)));
  CHECK_HIP_ERROR(hipMalloc(&db, size_b * sizeof(T)));
  CHECK_HIP_ERROR(hipMalloc(&dc, size_c * sizeof(T)));
  CHECK_HIP_ERROR(hipMalloc(&dd, size_d * sizeof(T)));
  if (enable_bias)
    CHECK_HIP_ERROR(hipMalloc(&d_bias, size_bias * sizeof(T)));
  // copy matrices from host to device
  CHECK_HIP_ERROR(
      hipMemcpy(da, ha.data(), sizeof(T) * size_a, hipMemcpyHostToDevice));
  CHECK_HIP_ERROR(
      hipMemcpy(db, hb.data(), sizeof(T) * size_b, hipMemcpyHostToDevice));
  CHECK_HIP_ERROR(
      hipMemcpy(dc, hc.data(), sizeof(T) * size_c, hipMemcpyHostToDevice));
  if (enable_bias)
    CHECK_HIP_ERROR(hipMemcpy(d_bias, h_bias.data(), sizeof(T) * size_bias,
                              hipMemcpyHostToDevice));

  hipblasLtHandle_t handle;
  hipblasLtMatrixLayout_t matA, matB, matC, matD;
  hipblasLtMatmulDesc_t matmul;
  hipblasLtMatmulPreference_t pref;
  uint64_t workspace_size = 1024 * 1024;
  void *d_worksapce;
  CHECK_HIP_ERROR(hipMalloc(&d_worksapce, workspace_size));

  CHECK_HIPBLASLT_ERROR(hipblasLtCreate(&handle));

  CHECK_HIPBLASLT_ERROR(
      hipblasLtMatrixLayoutCreate(&matA, in_out_datatype, row_a, col_a, lda));
  CHECK_HIPBLASLT_ERROR(
      hipblasLtMatrixLayoutCreate(&matB, in_out_datatype, row_b, col_b, ldb));
  CHECK_HIPBLASLT_ERROR(
      hipblasLtMatrixLayoutCreate(&matC, in_out_datatype, row_c, col_c, ldc));
  CHECK_HIPBLASLT_ERROR(
      hipblasLtMatrixLayoutCreate(&matD, in_out_datatype, row_c, col_c, ldd));

  CHECK_HIPBLASLT_ERROR(
      hipblasLtMatmulDescCreate(&matmul, HIPBLASLT_COMPUTE_F32, HIP_R_32F));

  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulDescSetAttribute(
      matmul, HIPBLASLT_MATMUL_DESC_TRANSA, &trans_a, sizeof(int32_t)));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulDescSetAttribute(
      matmul, HIPBLASLT_MATMUL_DESC_TRANSB, &trans_b, sizeof(int32_t)));

  hipblasLtEpilogue_t epilogue;
  if (enable_bias && actType == ActivationType::NONE)
    epilogue = HIPBLASLT_EPILOGUE_BIAS;
  else if (enable_bias && actType == ActivationType::RELU)
    epilogue = HIPBLASLT_EPILOGUE_RELU_BIAS;
  else if (enable_bias && actType == ActivationType::GELU)
    epilogue = HIPBLASLT_EPILOGUE_GELU_BIAS;
  else if (!enable_bias && actType == ActivationType::NONE)
    epilogue = HIPBLASLT_EPILOGUE_DEFAULT;
  else if (!enable_bias && actType == ActivationType::RELU)
    epilogue = HIPBLASLT_EPILOGUE_RELU;
  else if (!enable_bias && actType == ActivationType::GELU)
    epilogue = HIPBLASLT_EPILOGUE_GELU;
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulDescSetAttribute(
      matmul, HIPBLASLT_MATMUL_DESC_EPILOGUE, &epilogue, sizeof(epilogue)));
  if (enable_bias)
    CHECK_HIPBLASLT_ERROR(hipblasLtMatmulDescSetAttribute(
        matmul, HIPBLASLT_MATMUL_DESC_BIAS_POINTER, &d_bias, sizeof(void *)));

  // Set User Preference attributes
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulPreferenceCreate(&pref));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulPreferenceSetAttribute(
      pref, HIPBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &workspace_size,
      sizeof(workspace_size)));

  // Get Heuristic results
  hipblasLtMatmulHeuristicResult_t heuristicResult[3] = {0};
  int returnedAlgoCount = 0;
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulAlgoGetHeuristic(
      handle, matmul, matA, matB, matC, matD, pref, 3, heuristicResult,
      &returnedAlgoCount));

  // Solve problem
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmul(
      handle, matmul, &alpha, da, matA, db, matB, &beta, dc, matC, dd, matD,
      &heuristicResult[0].algo, d_worksapce, workspace_size, stream));

  hipStreamSynchronize(stream);
  // copy output from device to CPU
  CHECK_HIP_ERROR(
      hipMemcpy(hd.data(), dd, sizeof(T) * size_c, hipMemcpyDeviceToHost));

  std::cout << m << ", " << n << ", " << k << ", " << lda << ", " << ldb << ", "
            << ldc << ", " << stride_a << ", " << stride_b << ", " << stride_c
            << ", " << batch_count << ", " << alpha << ", " << beta << ", "
            << enable_bias << ", " << ToString(actType);
  if (timing) {
    hipEvent_t start, stop;
    hipEventCreate(&start);
    hipEventCreate(&stop);
    float eventMs = 1.0f;
    hipEventRecord(start, stream);
    for (int loop = 0; loop < BENCH_LOOP_COUNT; loop++) {
      CHECK_HIPBLASLT_ERROR(hipblasLtMatmul(
          handle, matmul, &alpha, da, matA, db, matB, &beta, dc, matC, dd, matD,
          &heuristicResult[0].algo, d_worksapce, workspace_size, stream));
    }
    hipEventRecord(stop, stream);
    hipEventSynchronize(stop);
    hipEventElapsedTime(&eventMs, start, stop);
    hipEventDestroy(start);
    hipEventDestroy(stop);
    eventMs /= BENCH_LOOP_COUNT;
    double flops = 2 * m * n * k * batch_count;
    double tflops = flops / eventMs / 1000000000;
    std::cout << ", " << eventMs << ", " << tflops;
  }
  std::cout << std::endl;

  // calculate golden or correct result
  if (validate) {
    auto *a_ptr = &ha[0];
    auto *b_ptr = &hb[0];
    auto *c_ptr = &hc[0];
    auto *d_ptr = &hd_gold[0];
    T *bias_ptr;
    if (enable_bias)
      bias_ptr = &h_bias[0];
    else
      bias_ptr = nullptr;
    mat_mat_mult<T, T, float>(alpha, beta, m, n, k, batch_count, a_ptr,
                              a_stride_1, a_stride_2, stride_a, b_ptr,
                              b_stride_1, b_stride_2, stride_b, c_ptr, 1, ldc,
                              stride_c, d_ptr, 1, ldd, stride_d, bias_ptr);
    if (actType != ActivationType::NONE)
      activation(m, n, ldd, d_ptr, actType);

    bool passed = true;
    for (int i = 0; i < size_c; i++) {
      if (!AlmostEqual(hd_gold[i], hd[i])) {
        printf("Err: Index %d: %f vs %f\n", i, static_cast<float>(hd_gold[i]),
               static_cast<float>(hd[i]));
        passed = false;
      }
    }
    if (!passed) {
      std::cout << "FAIL" << std::endl;
    } else {
      std::cout << "PASS" << std::endl;
    }
  }

  if (verbose) {
    printf("\n");
    if (trans_a == HIPBLASLT_OP_N) {
      print_strided_batched("ha initial", &ha[0], m, k, batch_count, 1, lda,
                            stride_a);
    } else {
      print_strided_batched("ha initial", &ha[0], m, k, batch_count, lda, 1,
                            stride_a);
    }
    if (trans_b == HIPBLASLT_OP_N) {
      print_strided_batched("hb initial", &hb[0], k, n, batch_count, 1, ldb,
                            stride_b);
    } else {
      print_strided_batched("hb initial", &hb[0], k, n, batch_count, ldb, 1,
                            stride_b);
    }
    print_strided_batched("hc initial", &hc[0], m, n, batch_count, 1, ldc,
                          stride_c);
    if (enable_bias)
      print_strided_batched("h_bias", &h_bias[0], m, 1, batch_count, 1, m, 0);
    print_strided_batched("hd_gold", &hd_gold[0], m, n, batch_count, 1, ldc,
                          stride_c);
    print_strided_batched("hd device", &hd[0], m, n, batch_count, 1, ldc,
                          stride_c);
  }

  CHECK_HIP_ERROR(hipFree(da));
  CHECK_HIP_ERROR(hipFree(db));
  CHECK_HIP_ERROR(hipFree(dc));
  CHECK_HIP_ERROR(hipFree(dd));
  CHECK_HIP_ERROR(hipFree(d_worksapce));
  if (enable_bias)
    CHECK_HIP_ERROR(hipFree(d_bias));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulPreferenceDestroy(pref));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatmulDescDestroy(matmul));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatrixLayoutDestory(matA));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatrixLayoutDestory(matB));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatrixLayoutDestory(matC));
  CHECK_HIPBLASLT_ERROR(hipblasLtMatrixLayoutDestory(matD));
  CHECK_HIPBLASLT_ERROR(hipblasLtDestroy(handle));

  return;
}

int main(int argc, char *argv[]) {
  // initialize parameters with default values
  hipblasLtOperation_t trans_a = HIPBLASLT_OP_N;
  hipblasLtOperation_t trans_b = HIPBLASLT_OP_N;
  hipDataType in_out_datatype = HIP_R_32F;

  int64_t invalid_int = std::numeric_limits<int64_t>::min() + 1;
  float invalid_float = std::numeric_limits<float>::quiet_NaN();

  // initialize to invalid value to detect if values not specified on command
  // line
  int64_t m = invalid_int, lda = invalid_int, stride_a = invalid_int;
  int64_t n = invalid_int, ldb = invalid_int, stride_b = invalid_int;
  int64_t k = invalid_int, ldc = invalid_int, stride_c = invalid_int;
  int64_t ldd = invalid_int, stride_d = invalid_int;

  int batch_count = BATCH_COUNT;

  float alpha = ALPHA;
  float beta = BETA;
  bool enable_bias = false;
  ActivationType actType = ActivationType::NONE;

  bool verbose = false;
  bool header = true;
  bool validate = false;
  bool timing = true;

  if (parse_arguments(argc, argv, in_out_datatype, m, n, k, lda, ldb, ldc, ldd,
                      stride_a, stride_b, stride_c, stride_d, batch_count,
                      alpha, beta, trans_a, trans_b, enable_bias, actType,
                      header, verbose, validate, timing)) {
    show_usage(argv);
    return EXIT_FAILURE;
  }

  // when arguments not specified, set to default values
  if (m == invalid_int)
    m = DIM1;
  if (n == invalid_int)
    n = DIM2;
  if (k == invalid_int)
    k = DIM3;
  if (lda == invalid_int)
    lda = trans_a == HIPBLASLT_OP_N ? m : k;
  if (ldb == invalid_int)
    ldb = trans_b == HIPBLASLT_OP_N ? k : n;
  if (ldc == invalid_int)
    ldc = m;
  if (ldd == invalid_int)
    ldd = m;
  if (stride_a == invalid_int)
    stride_a = trans_a == HIPBLASLT_OP_N ? lda * k : lda * m;
  if (stride_b == invalid_int)
    stride_b = trans_b == HIPBLASLT_OP_N ? ldb * n : ldb * k;
  if (stride_c == invalid_int)
    stride_c = ldc * n;
  if (stride_d == invalid_int)
    stride_d = ldd * n;
  if (alpha != alpha)
    alpha = ALPHA; // check for alpha == invalid_float == NaN
  if (beta != beta)
    beta = BETA; // check for beta == invalid_float == NaN
  if (batch_count == invalid_int)
    batch_count = BATCH_COUNT;

  if (bad_argument(trans_a, trans_b, m, n, k, lda, ldb, ldc, ldd, stride_a,
                   stride_b, stride_c, stride_d, batch_count)) {
    show_usage(argv);
    return EXIT_FAILURE;
  }

  if (header) {
    std::cout << "transAB, M, N, K, lda, ldb, ldc, stride_a, stride_b, "
                 "stride_c, batch_count, alpha, beta, bias, activationType";
    if (timing)
      std::cout << ", ms, tflops";
    std::cout << std::endl;
  }

  if (in_out_datatype == HIP_R_32F)
    test_hipblaslt<hipblasLtFloat>(
        in_out_datatype, trans_a, trans_b, m, n, k, lda, ldb, ldc, ldd,
        stride_a, stride_b, stride_c, stride_d, batch_count, alpha, beta,
        enable_bias, actType, validate, verbose, timing);
  else if (in_out_datatype == HIP_R_16F)
    test_hipblaslt<hipblasLtHalf>(
        in_out_datatype, trans_a, trans_b, m, n, k, lda, ldb, ldc, ldd,
        stride_a, stride_b, stride_c, stride_d, batch_count, alpha, beta,
        enable_bias, actType, validate, verbose, timing);

  return EXIT_SUCCESS;
}