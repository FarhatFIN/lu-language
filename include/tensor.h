#ifndef LU_TENSOR_H
#define LU_TENSOR_H

/* Optional, header-only C11 library. Link with -lm for sigmoid.
 * Tensor is an owning handle to a dense row-major double matrix.
 * Constructors and value-producing operations allocate a new handle.
 * Release each handle once with tensor_free; assignment only aliases it.
 * Invalid shapes, indices, NULL operands, allocation failure and division
 * by zero print a diagnostic and exit(EXIT_FAILURE). tensor_free(NULL) is OK.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct lu_tensor_storage {
    int rows, cols;
    double *data;
} *Tensor;

static inline void lu_tensor_fail(const char *message) {
    fprintf(stderr, "tensor: %s\n", message);
    exit(EXIT_FAILURE);
}

static inline size_t lu_tensor_count(Tensor t) {
    if (!t || !t->data || t->rows <= 0 || t->cols <= 0)
        lu_tensor_fail("invalid or null tensor");
    return (size_t)t->rows * (size_t)t->cols;
}

static inline Tensor tensor_zeros(int rows, int cols) {
    if (rows <= 0 || cols <= 0) lu_tensor_fail("shape must be positive");
    if ((size_t)rows > SIZE_MAX / (size_t)cols / sizeof(double))
        lu_tensor_fail("shape overflows allocation size");
    Tensor t = malloc(sizeof(*t));
    if (!t) lu_tensor_fail("out of memory");
    t->rows = rows;
    t->cols = cols;
    t->data = calloc((size_t)rows * (size_t)cols, sizeof(*t->data));
    if (!t->data) {
        free(t);
        lu_tensor_fail("out of memory");
    }
    return t;
}

static inline void tensor_free(Tensor t) {
    if (!t) return;
    free(t->data);
    free(t);
}

static inline size_t lu_tensor_index(Tensor t, int row, int col) {
    (void)lu_tensor_count(t);
    if (row < 0 || col < 0 || row >= t->rows || col >= t->cols)
        lu_tensor_fail("index out of bounds");
    return (size_t)row * (size_t)t->cols + (size_t)col;
}

static inline double tensor_get(Tensor t, int row, int col) {
    size_t index = lu_tensor_index(t, row, col);
    return t->data[index];
}

static inline void tensor_set(Tensor t, int row, int col, double value) {
    size_t index = lu_tensor_index(t, row, col);
    t->data[index] = value;
}

static inline void tensor_fill(Tensor t, double value) {
    size_t count = lu_tensor_count(t);
    for (size_t k = 0; k < count; k++) t->data[k] = value;
}

static inline Tensor tensor_full(int rows, int cols, double value) {
    Tensor t = tensor_zeros(rows, cols);
    tensor_fill(t, value);
    return t;
}

static inline Tensor tensor_ones(int rows, int cols) {
    return tensor_full(rows, cols, 1.0);
}

static inline Tensor tensor_eye(int size) {
    Tensor t = tensor_zeros(size, size);
    for (int i = 0; i < size; i++) tensor_set(t, i, i, 1.0);
    return t;
}

static inline void lu_tensor_same_shape(Tensor a, Tensor b) {
    (void)lu_tensor_count(a);
    (void)lu_tensor_count(b);
    if (a->rows != b->rows || a->cols != b->cols)
        lu_tensor_fail("shape mismatch");
}

static inline Tensor lu_tensor_elementwise(Tensor a, Tensor b, char operation) {
    lu_tensor_same_shape(a, b);
    Tensor result = tensor_zeros(a->rows, a->cols);
    size_t count = lu_tensor_count(a);
    for (size_t k = 0; k < count; k++) {
        switch (operation) {
        case '+': result->data[k] = a->data[k] + b->data[k]; break;
        case '-': result->data[k] = a->data[k] - b->data[k]; break;
        case '*': result->data[k] = a->data[k] * b->data[k]; break;
        case '/':
            if (b->data[k] == 0.0) {
                tensor_free(result);
                lu_tensor_fail("division by zero");
            }
            result->data[k] = a->data[k] / b->data[k];
            break;
        default:
            tensor_free(result);
            lu_tensor_fail("unsupported elementwise operation");
        }
    }
    return result;
}

static inline Tensor tensor_add(Tensor a, Tensor b) {
    return lu_tensor_elementwise(a, b, '+');
}

static inline Tensor tensor_sub(Tensor a, Tensor b) {
    return lu_tensor_elementwise(a, b, '-');
}

static inline Tensor tensor_mul(Tensor a, Tensor b) {
    return lu_tensor_elementwise(a, b, '*');
}

static inline Tensor tensor_div(Tensor a, Tensor b) {
    return lu_tensor_elementwise(a, b, '/');
}

static inline Tensor tensor_scale(Tensor a, double scalar) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++) result->data[k] = a->data[k] * scalar;
    return result;
}

static inline Tensor tensor_matmul(Tensor a, Tensor b) {
    (void)lu_tensor_count(a);
    (void)lu_tensor_count(b);
    if (a->cols != b->rows) lu_tensor_fail("matmul shape mismatch");
    Tensor result = tensor_zeros(a->rows, b->cols);
    for (int i = 0; i < a->rows; i++) {
        for (int k = 0; k < a->cols; k++) {
            double left = a->data[(size_t)i * (size_t)a->cols + (size_t)k];
            for (int j = 0; j < b->cols; j++)
                result->data[(size_t)i * (size_t)b->cols + (size_t)j] +=
                    left * b->data[(size_t)k * (size_t)b->cols + (size_t)j];
        }
    }
    return result;
}

static inline Tensor tensor_transpose(Tensor a) {
    (void)lu_tensor_count(a);
    Tensor result = tensor_zeros(a->cols, a->rows);
    for (int i = 0; i < a->rows; i++)
        for (int j = 0; j < a->cols; j++)
            result->data[(size_t)j * (size_t)a->rows + (size_t)i] =
                a->data[(size_t)i * (size_t)a->cols + (size_t)j];
    return result;
}

static inline Tensor tensor_relu(Tensor a) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++)
        result->data[k] = a->data[k] > 0.0 ? a->data[k] : 0.0;
    return result;
}

static inline Tensor tensor_sigmoid(Tensor a) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++)
        result->data[k] = 1.0 / (1.0 + exp(-a->data[k]));
    return result;
}

static inline double tensor_sum(Tensor t) {
    size_t count = lu_tensor_count(t);
    double result = 0.0;
    for (size_t k = 0; k < count; k++) result += t->data[k];
    return result;
}

static inline double tensor_mean(Tensor t) {
    size_t count = lu_tensor_count(t);
    return tensor_sum(t) / (double)count;
}

static inline int tensor_rows(Tensor t) {
    (void)lu_tensor_count(t);
    return t->rows;
}

static inline int tensor_cols(Tensor t) {
    (void)lu_tensor_count(t);
    return t->cols;
}

static inline void tensor_print(Tensor t) {
    (void)lu_tensor_count(t);
    printf("Tensor[%d,%d]\n", t->rows, t->cols);
    for (int i = 0; i < t->rows; i++) {
        printf("  [");
        for (int j = 0; j < t->cols; j++)
            printf("%s%g", j ? " " : "", t->data[(size_t)i * (size_t)t->cols + (size_t)j]);
        printf("]\n");
    }
}

#endif /* LU_TENSOR_H */
