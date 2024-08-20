# Copyright (c) 2024 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest

import numpy as np
from pass_test import PassTest

import paddle
from paddle.base import core


class TestDeformableConvTRTPattern(PassTest):
    def is_program_valid(self, program=None):
        return True

    def sample_program(self):
        with paddle.pir_utils.IrGuard():
            main_prog = paddle.static.Program()
            start_prog = paddle.static.Program()
            with paddle.pir.core.program_guard(main_prog, start_prog):
                x = paddle.static.data(
                    name='x', shape=[8, 1, 28, 28], dtype='float32'
                )
                kh, kw = 3, 3
                weight = paddle.static.data(
                    name='weight', shape=[16, 1, kh, kw], dtype='float32'
                )
                offset = paddle.static.data(
                    name='offset',
                    shape=[8, 2 * kh * kw, 26, 26],
                    dtype='float32',
                )
                deformable_conv_out = paddle.vision.ops.deform_conv2d(
                    x, offset, weight
                )
                out = paddle.assign(deformable_conv_out)
                self.pass_attr_list = [{'trt_op_marker_pass': {}}]
                self.feeds = {
                    "x": np.random.random([8, 1, 28, 28]).astype("float32"),
                    "weight": np.random.random([16, 1, kh, kw]).astype(
                        "float32"
                    ),
                    "offset": np.random.random([8, 2 * kh * kw, 26, 26]).astype(
                        "float32"
                    ),
                }
                self.fetch_list = [out]
                self.valid_op_map = {
                    "pd_op.fusion_transpose_flatten_concat": 0,
                }
                yield [main_prog, start_prog], False

    def setUp(self):
        if core.is_compiled_with_cuda():
            self.places.append(paddle.CUDAPlace(0))
        self.trt_expected_ops = {"pd_op.deformable_conv"}

    def test_check_output(self):
        self.check_pass_correct()


if __name__ == "__main__":
    unittest.main()
