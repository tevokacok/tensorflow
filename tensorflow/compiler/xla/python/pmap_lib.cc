/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/pmap_lib.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/notification.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "pybind11/cast.h"
#include "pybind11/pybind11.h"
#include "pybind11/pytypes.h"
#include "tensorflow/compiler/xla/python/absl_casters.h"
#include "tensorflow/compiler/xla/python/jax_jit.h"
#include "tensorflow/compiler/xla/python/py_buffer.h"
#include "tensorflow/compiler/xla/python/py_executable.h"
#include "tensorflow/compiler/xla/python/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/logging.h"

namespace jax {

namespace py = pybind11;

namespace {

struct PmapCacheEntry {
  // To get a first version running, we use extensively Python here for the
  // handling of the arguments and outputs.
  // TODO(jblespiau): Move more to C++.
  std::shared_ptr<xla::PyExecutable> executable;
  // See _cpp_pmap in api.py.
  py::object backend;
  // A function taking as argument a list of arguments and returns a list of
  // list of buffers `[num_devices x num_args]`.
  py::function handle_args;
  // A function taking as argument the output of `ExecuteShardedOnLocalDevices`
  // and returning a list of ShardedDeviceArray objects.
  py::function out_handler;
  xla::PyTreeDef out_pytree_def;

  // Ensures a single thread performs the compilation for a given executable.
  //
  // The first thread (holding the GIL) will create the CacheEntry associated to
  // a signature and if the object has been insterted already, other threads
  // will wait for the notification.
  absl::Notification compilation_complete;
  absl::optional<xla::Status> compilation_error = absl::nullopt;

  bool fall_back_to_python = false;
};

}  // namespace

// A `PmapFunction` is associated to a `jax.pmap(f)` and takes care of the
// bookkeeping of the different signatures used and the dispatch of calls to
// the correct underlying `PyExecutable`. This class is thread-safe.
class PmapFunction {
 public:
  PmapFunction(py::function fun, py::function cache_miss,
               std::vector<int> static_argnums)
      : fun_(std::move(fun)),
        cache_miss_(std::move(cache_miss)),
        static_argnums_(std::move(static_argnums)) {
    std::sort(static_argnums_.begin(), static_argnums_.end());
  }

  // This function will:
  // (a) flatten the inputs using pytree
  // (b) get buffer objects from the arguments
  // (c) call the executable
  // (d) construct `ShardedDeviceArray` objects from the outputs
  // (e) reconstruct the `PyTree`.
  py::object Call(py::args args, py::kwargs kwargs);

  py::object PythonSignature() {
    static const auto* inspect = new py::module(py::module::import("inspect"));
    return inspect->attr("signature")(fun_);
  }

  int cache_size() const { return executables_.size(); }

 private:
  // Returns nullptr if not present in the cache.
  PmapCacheEntry* GetCacheEntryIfPresent(const CallSignature& signature);
  // Should never return nullptr.
  PmapCacheEntry* AddCacheEntry(const py::args& args, const py::kwargs& kwargs,
                                const CallSignature& signature,
                                py::object out_and_fastpath_data);

  bool always_fallback_to_python_ = false;

  const py::function fun_;  // The Python function to pmap.
  // See JAX _cpp_pmap in api.py for documentation.
  const py::function cache_miss_;

  // We need to know the static arguments to remove them from the arguments
  // passed to the underlying PyExecutable. In sorted order.
  std::vector<int> static_argnums_;
  // We need a `unique_ptr` here to ensure value pointer stability.
  absl::flat_hash_map<CallSignature, std::unique_ptr<PmapCacheEntry>>
      executables_;

  // A vector of size `num_outputs`, specifying the sharding of each output
  std::vector<ShardingSpec> sharding_specs_;
};

PmapCacheEntry* PmapFunction::GetCacheEntryIfPresent(
    const CallSignature& signature) {
  auto found_iterator = executables_.find(signature);
  if (found_iterator != executables_.end()) {  // Cache hit!
    if (!found_iterator->second->compilation_complete.HasBeenNotified()) {
      py::gil_scoped_release gil_release;
      found_iterator->second->compilation_complete.WaitForNotification();
    }
    if (found_iterator->second->compilation_error) {
      throw std::invalid_argument(
          found_iterator->second->compilation_error.value().error_message());
    }
    return found_iterator->second.get();
  }
  return nullptr;
}

PmapCacheEntry* PmapFunction::AddCacheEntry(const py::args& args,
                                            const py::kwargs& kwargs,
                                            const CallSignature& signature,
                                            py::object out_and_fastpath_data) {
  // We need to insert the element.
  auto result =
      executables_.emplace(signature, std::make_unique<PmapCacheEntry>());
  auto it = result.first;
  PmapCacheEntry* cache_entry = it->second.get();

  py::tuple tuple = py::cast<py::tuple>(out_and_fastpath_data);
  CHECK_EQ(tuple.size(), 2);
  if (tuple[1].is_none()) {
    cache_entry->fall_back_to_python = true;
    cache_entry->compilation_complete.Notify();
    return cache_entry;
  }

  py::tuple pmap_data = py::cast<py::tuple>(tuple[1]);
  if (py::cast<int>(pmap_data.attr("version")) != 1) {
    throw std::runtime_error(absl::StrCat(
        "The versions of jaxlib and Jax are incompatible (pmap cpp version 1 "
        "expected, but got ",
        py::cast<int>(pmap_data.attr("version")),
        "Upgrade jaxlib and jax. Provided data was:",
        py::cast<std::string>(py::str(py::repr(pmap_data)))));
  }
  // See api.py::_PmapFastpathData in the JAX code base for the expected
  // namedtuple.
  auto executable = py::cast<std::shared_ptr<xla::PyExecutable>>(
      pmap_data.attr("xla_executable"));
  cache_entry->executable = std::move(executable);
  cache_entry->handle_args =
      py::cast<py::function>(pmap_data.attr("in_handler"));
  cache_entry->out_handler =
      py::cast<py::function>(pmap_data.attr("out_handler"));
  auto out_tree = py::cast<xla::PyTreeDef>(pmap_data.attr("out_pytree_def"));
  cache_entry->out_pytree_def = std::move(out_tree);

  cache_entry->compilation_complete.Notify();
  return cache_entry;
}

py::object PmapFunction::Call(py::args args, py::kwargs kwargs) {
  if (always_fallback_to_python_) {
    return py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0];
  }

  ParsedArgumentsAsBuffers arguments;
  if (!ParseArguments(args, kwargs, static_argnums_, /*static_argnames=*/{},
                      arguments)
           .ok()) {
    return py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0];
  }

  // Get dynamic argument signatures.
  GlobalJitState& global_state = jax::GetGlobalState();
  ThreadLocalJitState& tls = jax::GetLocalState();
  const bool jax_enable_x64 = tls.enable_x64.value_or(global_state.enable_x64);
  arguments.signature.jax_enable_x64 = jax_enable_x64;
  for (py::handle arg : arguments.flat_dynamic_args) {
    auto signature_or_error = xla::PyArgSignatureOfValue(arg, jax_enable_x64);
    if (!signature_or_error.ok()) {
      return py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0];
    }
    arguments.signature.dynamic_arg_signatures.push_back(
        std::move(signature_or_error).ValueOrDie());
  }
  arguments.signature.global_extra_jit_context = global_state.extra_jit_context;
  arguments.signature.thread_local_extra_jit_context = tls.extra_jit_context;

  // Retrieve/Maybe add the executable to the cache.
  PmapCacheEntry* cache_entry = GetCacheEntryIfPresent(arguments.signature);
  if (!cache_entry) {
    py::object out_and_fastpath_data = cache_miss_(*args, **kwargs);
    cache_entry = GetCacheEntryIfPresent(arguments.signature);
    if (!cache_entry) {
      cache_entry = AddCacheEntry(args, kwargs, arguments.signature,
                                  out_and_fastpath_data);
    }
    CHECK(cache_entry);
    if (cache_entry->fall_back_to_python) {
      return py::cast<py::tuple>(out_and_fastpath_data)[0];
    }
    // As we have already computed the results, we can return it.
    // It's even *required* e.g. if there are donated arguments, because
    // otherwise the buffer which has been donated already will be invalid.
    return py::cast<py::tuple>(out_and_fastpath_data)[0];
  }

  CHECK(cache_entry);
  if (cache_entry->fall_back_to_python) {
    return py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0];
  }

  // TODO(jblespiau): Use C++ only for this.
  py::list arg_list;
  for (auto& v : arguments.flat_dynamic_args) {
    arg_list.append(v);
  }

  py::object handled_args = cache_entry->handle_args(arg_list);
  // The Python shard_args returns `[num_args, num_devices]`.
  py::list list_of_list_of_buffers = py::cast<py::list>(handled_args);

  arguments.keep_alive_objects.push_back(
      py::cast<py::object>(list_of_list_of_buffers));
  // arg_buffers is `[num_args x num_devices]`.
  std::vector<std::vector<xla::PyBuffer::object>> arg_buffers;
  const int num_args = list_of_list_of_buffers.size();
  arg_buffers.reserve(num_args);
  for (int i = 0; i < num_args; ++i) {
    std::vector<xla::PyBuffer::object> buffers;
    buffers.reserve(py::cast<py::list>(list_of_list_of_buffers[i]).size());
    for (auto& buf : list_of_list_of_buffers[i]) {
      buffers.push_back(py::cast<xla::PyBuffer::object>(buf));
    }
    arg_buffers.push_back(std::move(buffers));
  }

  // TODO(jblespiau): `ExecuteShardedOnLocalDevices` performs an inversion of
  // the [args, num_devices] axis. When moving shard_args to C++, we can prevent
  // this by calling Execute directly.
  // A vector of [num_outputs, num_devices].
  std::vector<std::vector<xla::PyBuffer::object>> outputs = ValueOrThrow(
      cache_entry->executable->ExecuteShardedOnLocalDevices(arg_buffers));

  // TODO(jblespiau): Move this to C++.
  py::list outputs_as_python_objects;
  for (int i = 0; i < outputs.size(); ++i) {
    outputs_as_python_objects.append(py::cast(std::move(outputs[i])));
  }
  py::list flat_sharded_device_arrays =
      cache_entry->out_handler(outputs_as_python_objects);
  return cache_entry->out_pytree_def.Unflatten(flat_sharded_device_arrays);
}

void BuildPmapSubmodule(py::module& m) {
  py::module pmap_lib = m.def_submodule("pmap_lib", "Jax C++ pmap library");

  py::class_<NoSharding> no_sharding(pmap_lib, "NoSharding");
  no_sharding.def(py::init<>())
      .def("__repr__",
           [](const NoSharding& chuncked) { return "NoSharding()"; })
      .def("__eq__",
           [](const NoSharding& self, py::object obj) {
             return py::isinstance<NoSharding>(obj);
           })
      .def("__hash__", [](const NoSharding& self) {
        const size_t hash = absl::Hash<NoSharding>()(self);
        return py::int_(hash);
      });

  py::class_<Chunked> chunked(pmap_lib, "Chunked");
  chunked.def(py::init<std::vector<int>>())
      .def_readonly("chunks", &Chunked::chunks)
      .def("__repr__",
           [](const Chunked& chuncked) {
             return absl::StrCat("Chunked(",
                                 absl::StrJoin(chuncked.chunks, ","), ")");
           })
      .def("__eq__", [](const Chunked& self, py::object other) {
        if (!py::isinstance<Chunked>(other)) {
          return false;
        }
        return self == py::cast<const Chunked&>(other);
      });

  py::class_<Unstacked> unstacked(pmap_lib, "Unstacked");
  unstacked.def(py::init<int>())
      .def_readonly("size", &Unstacked::size)
      .def("__repr__",
           [](const Unstacked& x) {
             return absl::StrCat("Unstacked(", x.size, ")");
           })
      .def("__eq__", [](const Unstacked& self, py::object other) {
        if (!py::isinstance<Unstacked>(other)) {
          return false;
        }
        return self == py::cast<const Unstacked&>(other);
      });

  py::class_<ShardedAxis> sharded_axis(pmap_lib, "ShardedAxis");
  sharded_axis.def(py::init<int>()).def_readonly("axis", &ShardedAxis::axis);
  sharded_axis
      .def("__repr__",
           [](const ShardedAxis& x) {
             return absl::StrCat("ShardedAxis(axis=", x.axis, ")");
           })
      .def("__eq__", [](const ShardedAxis& self, const ShardedAxis& other) {
        return self == other;
      });

  py::class_<Replicated> replicated(pmap_lib, "Replicated");
  replicated.def(py::init<int>())
      .def_readonly("replicas", &Replicated::replicas)
      .def("__repr__",
           [](const Replicated& x) {
             return absl::StrCat("Replicated(replicas=", x.replicas, ")");
           })
      .def("__eq__", [](const Replicated& self, const Replicated& other) {
        return self == other;
      });

  py::class_<ShardingSpec> sharding_spec(pmap_lib, "ShardingSpec");
  sharding_spec
      .def(py::init<py::iterable, py::iterable>(), py::arg("sharding"),
           py::arg("mesh_mapping"))
      .def_property_readonly(
          "sharding",
          [](const ShardingSpec& self) {
            return xla::SpanToTuple(absl::MakeConstSpan(self.GetSharding()));
          })
      .def_property_readonly(
          "mesh_mapping",
          [](const ShardingSpec& self) {
            return xla::SpanToTuple(absl::MakeConstSpan(self.GetMeshMapping()));
          })
      .def("__eq__", [](const ShardingSpec& self,
                        const ShardingSpec& other) { return self == other; })
      .def("__hash__", [](const ShardingSpec& self) {
        const size_t hash = absl::Hash<ShardingSpec>()(self);
        return py::int_(hash);
      });

  py::class_<ShardedDeviceArrayBase> sda_base(pmap_lib,
                                              "ShardedDeviceArrayBase");
  sda_base.def(py::init<>());

  py::class_<ShardedDeviceArray, ShardedDeviceArrayBase> sda(
      pmap_lib, "ShardedDeviceArray");
  sda.def(py::init<py::handle, ShardingSpec, py::list, py::handle>(),
          py::arg("aval"), py::arg("sharding_spec"), py::arg("device_buffers"),
          py::arg("indices"))
      .def_property_readonly("aval", &ShardedDeviceArray::aval)
      .def_property_readonly("indices", &ShardedDeviceArray::indices)
      .def_property_readonly("sharding_spec",
                             &ShardedDeviceArray::GetShardingSpec)
      .def_property("device_buffers", &ShardedDeviceArray::device_buffers,
                    &ShardedDeviceArray::set_device_buffers)
      .def_property("_npy_value", &ShardedDeviceArray::npy_value,
                    &ShardedDeviceArray::set_npy_value)
      .def_property("_one_replica_buffer_indices",
                    &ShardedDeviceArray::one_replica_buffer_indices,
                    &ShardedDeviceArray::set_one_replica_buffer_indices)
      .def_property_readonly("shape",
                             [](const ShardedDeviceArray& self) {
                               return self.aval().attr("shape");
                             })
      .def_property_readonly("dtype",
                             [](const ShardedDeviceArray& self) {
                               return self.aval().attr("dtype");
                             })
      .def_property_readonly(
          "size",
          [](const ShardedDeviceArray& self) {
            py::tuple shape = py::cast<py::tuple>(self.aval().attr("shape"));
            int size = 1;
            for (auto dim : shape) {
              size *= py::cast<int>(dim);
            }
            return size;
          })
      .def_property_readonly("ndim", [](const ShardedDeviceArray& self) {
        return py::len(self.aval().attr("shape"));
      });

  py::class_<PmapFunction, std::unique_ptr<PmapFunction>> cfun(pmap_lib,
                                                               "PmapFunction");
  cfun.def("__call__", &PmapFunction::Call);
  cfun.def_property_readonly("__signature__", &PmapFunction::PythonSignature);
  // All private members are only for testing/debugging purposes
  cfun.def("_cache_size", &PmapFunction::cache_size);

  pmap_lib.def(
      "pmap",
      [](py::function fun, py::function cache_miss,
         std::vector<int> static_argnums) -> std::unique_ptr<PmapFunction> {
        return std::make_unique<PmapFunction>(
            std::move(fun), std::move(cache_miss), std::move(static_argnums));
      });
}

}  // namespace jax
