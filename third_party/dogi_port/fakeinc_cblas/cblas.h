#pragma once
// Minimal row-major sgemm for DOGI's MLP inference (C = alpha*op(A)*op(B) + beta*C).
typedef enum { CblasRowMajor = 101 } CBLAS_LAYOUT;
typedef enum { CblasNoTrans = 111, CblasTrans = 112 } CBLAS_TRANSPOSE;
#ifdef __cplusplus
extern "C" {
#endif
void cblas_sgemm(CBLAS_LAYOUT layout, CBLAS_TRANSPOSE ta, CBLAS_TRANSPOSE tb,
                 int m, int n, int k, float alpha, const float* A, int lda,
                 const float* B, int ldb, float beta, float* C, int ldc);
#ifdef __cplusplus
}
#endif
