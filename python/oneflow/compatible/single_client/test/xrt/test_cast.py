"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import unittest

import numpy as np

import oneflow.compatible.single_client.unittest
from oneflow.compatible import single_client as flow

config = flow.function_config()


def make_job(input_shape, dtype=flow.float32, target_dtype=flow.float32):
    config.use_xla_jit(False)
    config.use_tensorrt(False)

    @flow.global_function(config)
    def cast_job(x=flow.FixedTensorDef(input_shape, dtype=dtype)):
        return flow.cast(x, dtype=target_dtype)

    return cast_job


def make_xla_job(input_shape, dtype=flow.float32, target_dtype=flow.float32):
    config.use_xla_jit(True)
    config.use_tensorrt(False)

    @flow.global_function(config)
    def xla_cast_job(x=flow.FixedTensorDef(input_shape, dtype=dtype)):
        return flow.cast(x, dtype=target_dtype)

    return xla_cast_job


class TestCast(unittest.TestCase):
    def _test_body(self, x, dtype=flow.float32, target_dtype=flow.float32):
        f1 = make_job(x.shape, dtype=dtype, target_dtype=target_dtype)
        f2 = make_xla_job(x.shape, dtype=dtype, target_dtype=target_dtype)
        a = f1(x).get()
        b = f2(x).get()
        print("without xla: ", a)
        print("with xla", b)
        self.assertTrue(np.allclose(a.numpy(), b.numpy(), rtol=0.001, atol=1e-05))
        flow.clear_default_session()

    def _test_ones_body(self, shape, dtype=flow.float32, target_dtype=flow.float32):
        np_dtype = flow.convert_oneflow_dtype_to_numpy_dtype(dtype)
        x = np.ones(shape, dtype=np_dtype)
        self._test_body(x, dtype=dtype, target_dtype=target_dtype)

    def _test_random_body(self, shape, dtype=flow.float32, target_dtype=flow.float32):
        np_dtype = flow.convert_oneflow_dtype_to_numpy_dtype(dtype)
        x = (1000 * np.random.random(shape)).astype(np_dtype)
        self._test_body(x, dtype=dtype, target_dtype=target_dtype)

    def test_ones_input(self):
        self._test_ones_body(1, flow.float32, flow.int32)
        self._test_ones_body((1, 10), flow.int32, flow.float32)

    def test_random_input(self):
        self._test_random_body(1, flow.float32, flow.int32)
        self._test_random_body((1, 10), flow.int32, flow.float32)


if __name__ == "__main__":
    unittest.main()
