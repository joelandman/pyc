def strides_from_shape(ndim, shape, itemsize):
    strides = list(shape[1:]) + [itemsize]
    for i in range(ndim - 2, -1, -1):
        strides[i] *= strides[i + 1]
    return strides

print(strides_from_shape(3, [3, 2, 5], 1))
