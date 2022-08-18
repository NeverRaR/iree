// RUN: iree-opt --split-input-file --iree-hal-inline-conversion %s | FileCheck %s

// CHECK-LABEL: @buffer_view_create
func.func @buffer_view_create(%arg0: !hal.buffer, %arg1: index, %arg2: index) -> !hal.buffer_view {
  %c1 = arith.constant 1 : i32
  %c32 = arith.constant 32 : i32
  // CHECK: %view = hal_inline.buffer_view.create
  // CHECK-SAME: buffer(%arg0 : !hal.buffer)
  // CHECK-SAME: shape([%arg1, %arg2])
  // CHECK-SAME: type(%c32_i32)
  // CHECK-SAME: encoding(%c1_i32) : !hal.buffer_view
  %view = hal.buffer_view.create buffer(%arg0 : !hal.buffer)
                                 shape([%arg1, %arg2])
                                 type(%c32)
                                 encoding(%c1) : !hal.buffer_view
  return %view : !hal.buffer_view
}

// -----

// CHECK-LABEL: @buffer_view_buffer
func.func @buffer_view_buffer(%arg0: !hal.buffer_view) -> !hal.buffer {
  // CHECK: %buffer = hal_inline.buffer_view.buffer<%arg0 : !hal.buffer_view> : !hal.buffer
  %buffer = hal.buffer_view.buffer<%arg0 : !hal.buffer_view> : !hal.buffer
  return %buffer : !hal.buffer
}

// -----

// CHECK-LABEL: @buffer_view_shape_queries
func.func @buffer_view_shape_queries(%arg0: !hal.buffer_view) -> (index, index) {
  // CHECK: %{{.+}} = hal_inline.buffer_view.rank<%arg0 : !hal.buffer_view> : index
  %0 = hal.buffer_view.rank<%arg0 : !hal.buffer_view> : index
  // CHECK: %{{.+}} = hal_inline.buffer_view.dim<%arg0 : !hal.buffer_view>[0] : index
  %1 = hal.buffer_view.dim<%arg0 : !hal.buffer_view>[0] : index
  return %0, %1 : index, index
}
