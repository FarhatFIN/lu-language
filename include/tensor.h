#ifndef LU_TENSOR_H
#define LU_TENSOR_H

/* Optional, header-only C11 library. Link with -lm for sigmoid and tanh.
 * Tensor is an owning handle to a dense row-major double matrix.
 * Constructors and value-producing operations allocate a new handle.
 * Release each handle once with tensor_free; assignment only aliases it.
 * Invalid shapes, indices, NULL operands, allocation failure and division
 * by zero print a diagnostic and exit(EXIT_FAILURE). tensor_free(NULL) is OK.
 *
 * This is the C backend's library: `luc -c` emits `#include <tensor.h>`
 * verbatim, so build with `cc -std=c11 -Iinclude out.c -lm`.
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

/* Diagnostics name the shapes involved: with an owning handle the mistake is
   almost always a shape or an index, and "shape mismatch" alone does not say
   which operand was wrong. */
static inline void lu_tensor_fail_shapes(const char *what, Tensor a, Tensor b) {
    char message[160];
    snprintf(message, sizeof message, "%s: shape mismatch [%d,%d] vs [%d,%d]",
             what, a->rows, a->cols, b->rows, b->cols);
    lu_tensor_fail(message);
}

static inline size_t lu_tensor_count(Tensor t) {
    if (!t || !t->data || t->rows <= 0 || t->cols <= 0)
        lu_tensor_fail("invalid or null tensor");
    return (size_t)t->rows * (size_t)t->cols;
}

static inline Tensor tensor_zeros(int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        char message[96];
        snprintf(message, sizeof message, "shape must be positive, got [%d,%d]",
                 rows, cols);
        lu_tensor_fail(message);
    }
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
    if (row < 0 || col < 0 || row >= t->rows || col >= t->cols) {
        char message[160];
        snprintf(message, sizeof message,
                 "index out of bounds: [%d,%d] not inside [%d,%d]",
                 row, col, t->rows, t->cols);
        lu_tensor_fail(message);
    }
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

/* Deep copy: assignment aliases the handle, so duplicating a tensor needs
   an explicit call. */
static inline Tensor tensor_copy(Tensor a) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++) result->data[k] = a->data[k];
    return result;
}

static inline void lu_tensor_same_shape(Tensor a, Tensor b) {
    (void)lu_tensor_count(a);
    (void)lu_tensor_count(b);
    if (a->rows != b->rows || a->cols != b->cols)
        lu_tensor_fail_shapes("elementwise", a, b);
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
                char message[96];
                snprintf(message, sizeof message, "division by zero at [%d,%d]",
                         (int)(k / (size_t)a->cols), (int)(k % (size_t)a->cols));
                tensor_free(result);
                lu_tensor_fail(message);
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

/* i-k-j loop order: the inner loop walks a contiguous row of `b` and writes
   a contiguous row of the result, so both operands are read sequentially.
   `restrict` on the result is valid because it is freshly allocated and
   cannot alias the inputs; the inputs may alias each other. */
static inline Tensor tensor_matmul(Tensor a, Tensor b) {
    (void)lu_tensor_count(a);
    (void)lu_tensor_count(b);
    if (a->cols != b->rows) {
        char message[160];
        snprintf(message, sizeof message, "matmul shape mismatch: [%d,%d] x [%d,%d]",
                 a->rows, a->cols, b->rows, b->cols);
        lu_tensor_fail(message);
    }
    const int rows = a->rows, inner = a->cols, cols = b->cols;
    Tensor result = tensor_zeros(rows, cols);
    const double *src = a->data;
    const double *rhs = b->data;
    double *restrict dst = result->data;
    for (int i = 0; i < rows; i++) {
        const double *left_row = src + (size_t)i * (size_t)inner;
        double *out_row = dst + (size_t)i * (size_t)cols;
        for (int k = 0; k < inner; k++) {
            const double left = left_row[k];
            const double *right_row = rhs + (size_t)k * (size_t)cols;
            for (int j = 0; j < cols; j++) out_row[j] += left * right_row[j];
        }
    }
    return result;
}

/* Matrix times column vector: [rows,inner] x [inner,1] -> [rows,1]. */
static inline Tensor tensor_matvec(Tensor a, Tensor v) {
    (void)lu_tensor_count(a);
    (void)lu_tensor_count(v);
    if (v->cols != 1 || a->cols != v->rows) {
        char message[160];
        snprintf(message, sizeof message,
                 "matvec needs [%d,%d] x [%d,1], got [%d,%d] x [%d,%d]",
                 a->rows, a->cols, a->cols, a->rows, a->cols, v->rows, v->cols);
        lu_tensor_fail(message);
    }
    Tensor result = tensor_zeros(a->rows, 1);
    for (int i = 0; i < a->rows; i++) {
        double sum = 0.0;
        const double *row = a->data + (size_t)i * (size_t)a->cols;
        for (int k = 0; k < a->cols; k++) sum += row[k] * v->data[k];
        result->data[i] = sum;
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

static inline Tensor tensor_abs(Tensor a) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++)
        result->data[k] = fabs(a->data[k]);
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

static inline Tensor tensor_tanh(Tensor a) {
    size_t count = lu_tensor_count(a);
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++)
        result->data[k] = tanh(a->data[k]);
    return result;
}

/* Clamp every element into [low, high]; a reversed range is a bug, not an
   empty result. */
static inline Tensor tensor_clip(Tensor a, double low, double high) {
    size_t count = lu_tensor_count(a);
    if (!(low <= high)) lu_tensor_fail("clip needs low <= high");
    Tensor result = tensor_zeros(a->rows, a->cols);
    for (size_t k = 0; k < count; k++) {
        double value = a->data[k];
        if (value < low) value = low;
        if (value > high) value = high;
        result->data[k] = value;
    }
    return result;
}

/* Scalar product of two equally shaped tensors. */
static inline double tensor_dot(Tensor a, Tensor b) {
    size_t count = lu_tensor_count(b);
    if (count != lu_tensor_count(a) || a->rows != b->rows || a->cols != b->cols)
        lu_tensor_fail_shapes("dot", a, b);
    double result = 0.0;
    for (size_t k = 0; k < count; k++) result += a->data[k] * b->data[k];
    return result;
}

static inline double tensor_sum(Tensor t) {
    size_t count = lu_tensor_count(t);
    double result = 0.0;
    for (size_t k = 0; k < count; k++) result += t->data[k];
    return result;
}

static inline double tensor_max(Tensor t) {
    size_t count = lu_tensor_count(t);
    double result = t->data[0];
    for (size_t k = 1; k < count; k++)
        if (t->data[k] > result) result = t->data[k];
    return result;
}

static inline double tensor_min(Tensor t) {
    size_t count = lu_tensor_count(t);
    double result = t->data[0];
    for (size_t k = 1; k < count; k++)
        if (t->data[k] < result) result = t->data[k];
    return result;
}

/* Flat index (row-major) of the first maximum. */
static inline int tensor_argmax(Tensor t) {
    size_t count = lu_tensor_count(t);
    size_t best = 0;
    for (size_t k = 1; k < count; k++)
        if (t->data[k] > t->data[best]) best = k;
    return (int)best;
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
