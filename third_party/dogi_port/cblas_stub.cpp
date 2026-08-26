#include "fakeinc_cblas/cblas.h"
extern "C" void cblas_sgemm(CBLAS_LAYOUT, CBLAS_TRANSPOSE ta, CBLAS_TRANSPOSE tb,
                            int m, int n, int k, float alpha, const float* A, int lda,
                            const float* B, int ldb, float beta, float* C, int ldc) {
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j) {
            double acc = 0;
            for (int p = 0; p < k; ++p) {
                float a = (ta == CblasNoTrans) ? A[i * lda + p] : A[p * lda + i];
                float b = (tb == CblasNoTrans) ? B[p * ldb + j] : B[j * ldb + p];
                acc += (double)a * b;
            }
            C[i * ldc + j] = alpha * (float)acc + beta * C[i * ldc + j];
        }
}
