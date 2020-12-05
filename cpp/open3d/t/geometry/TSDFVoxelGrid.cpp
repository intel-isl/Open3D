// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/geometry/TSDFVoxelGrid.h"

#include "open3d/Open3D.h"
#include "open3d/core/kernel/Kernel.h"
#include "open3d/t/geometry/PointCloud.h"
#include "open3d/utility/Console.h"

namespace open3d {
namespace t {
namespace geometry {

TSDFVoxelGrid::TSDFVoxelGrid(
        std::unordered_map<std::string, int> attr_channel_map,
        float voxel_size,
        float sdf_trunc,
        int64_t block_resolution,
        int64_t block_count,
        const core::Device &device)
    : attr_channel_map_(attr_channel_map),
      voxel_size_(voxel_size),
      sdf_trunc_(sdf_trunc),
      block_resolution_(block_resolution),
      block_count_(block_count),
      device_(device) {
    int total_channels = 0;
    for (auto &kv : attr_channel_map_) {
        total_channels += kv.second;
    }
    core::Dtype key_dtype(core::Dtype::DtypeCode::Object,
                          core::Dtype::Int64.ByteSize() * 3, "_hash_k");
    core::Dtype val_dtype(core::Dtype::DtypeCode::Object,
                          core::Dtype::Float32.ByteSize() * block_resolution_ *
                                  block_resolution_ * block_resolution_ *
                                  total_channels,
                          "_hash_v");
    block_hashmap_ = std::make_shared<core::Hashmap>(block_count_, key_dtype,
                                                     val_dtype, device);
}

void TSDFVoxelGrid::Integrate(const Image &depth,
                              const core::Tensor &intrinsics,
                              const core::Tensor &extrinsics,
                              double depth_scale) {
    // Unproject
    PointCloud pcd =
            PointCloud::CreateFromDepthImage(depth, intrinsics, depth_scale);
    PointCloud pcd_plus = PointCloud::CreateFromDepthImage(
            Image(depth.AsTensor() + sdf_trunc_ * depth_scale), intrinsics,
            depth_scale);
    PointCloud pcd_minus = PointCloud::CreateFromDepthImage(
            Image(depth.AsTensor() - sdf_trunc_ * depth_scale), intrinsics,
            depth_scale);
    pcd = PointCloud(pcd.GetPoints() + pcd_plus.GetPoints() +
                     pcd_minus.GetPoints());
    pcd.Transform(extrinsics.Inverse());

    // Pre-compressing for blocks
    float block_size = voxel_size_ * block_resolution_;
    core::Tensor block_coordd = pcd.GetPoints().AsTensor() / block_size;
    core::Tensor block_coordi = block_coordd.To(core::Dtype::Int64);
    core::Hashmap pcd_block_hashmap(
            block_coordi.GetShape()[0],
            core::Dtype(core::Dtype::DtypeCode::Object,
                        core::Dtype::Int64.ByteSize() * 3, "_hash_k"),
            core::Dtype::Int64, device_);
    core::Tensor block_addrs, block_masks;
    pcd_block_hashmap.Activate(block_coordi, block_addrs, block_masks);

    // Activate in blocks
    core::Tensor block_coords = block_coordi.IndexGet({block_masks});
    core::Tensor addrs, masks;
    block_hashmap_->Activate(block_coords, addrs, masks);
    block_hashmap_->Find(block_coords, addrs, masks);

    // Input
    std::unordered_map<std::string, core::Tensor> srcs = {
            {"depth", depth.AsTensor()},
            {"indices", addrs.To(core::Dtype::Int64).IndexGet({masks})},
            {"block_keys",
             core::Hashmap::ReinterpretBufferTensor(
                     block_hashmap_->GetKeyTensor(),
                     {block_hashmap_->GetCapacity(), 3}, core::Dtype::Int64)},
            {"intrinsics", intrinsics.Copy(device_)},
            {"extrinsics", extrinsics.Copy(device_)},
            {"resolution", core::Tensor(std::vector<int64_t>{block_resolution_},
                                        {}, core::Dtype::Int64, device_)},
            {"depth_scale",
             core::Tensor(std::vector<float>{static_cast<float>(depth_scale)},
                          {}, core::Dtype::Float32, device_)},
            {"voxel_size", core::Tensor(std::vector<float>{voxel_size_}, {},
                                        core::Dtype::Float32, device_)},
            {"sdf_trunc", core::Tensor(std::vector<float>{sdf_trunc_}, {},
                                       core::Dtype::Float32, device_)}};

    // In-place modified output
    std::unordered_map<std::string, core::Tensor> dsts = {
            {"block_values",
             core::Hashmap::ReinterpretBufferTensor(
                     block_hashmap_->GetValueTensor(),
                     {block_hashmap_->GetCapacity(), block_resolution_,
                      block_resolution_, block_resolution_, 2},
                     core::Dtype::Float32)}};

    core::kernel::GeneralEW(srcs, dsts,
                            core::kernel::GeneralEWOpCode::TSDFIntegrate);

    // Debug section
    // core::Tensor active_addrs({block_hashmap_->Size()}, core::Dtype::Int32,
    //                           device_);
    // block_hashmap_->GetActiveIndices(
    //         static_cast<core::addr_t *>(active_addrs.GetDataPtr()));
    // core::Tensor active_values =
    //         core::Hashmap::ReinterpretBufferTensor(
    //                 block_hashmap_->GetValueTensor(),
    //                 {block_hashmap_->GetCapacity(), block_resolution_,
    //                  block_resolution_, block_resolution_, 2},
    //                 core::Dtype::Float32)
    //                 .IndexGet({active_addrs.To(core::Dtype::Int64)});
    // utility::LogInfo("{}", active_values.ToString());
    // utility::LogInfo("Active blocks in hashmap = {}",
    // block_hashmap_->Size());
}

PointCloud TSDFVoxelGrid::ExtractSurface() {
    core::Tensor active_addrs({block_hashmap_->Size()}, core::Dtype::Int32,
                              device_);
    block_hashmap_->GetActiveIndices(
            static_cast<core::addr_t *>(active_addrs.GetDataPtr()));
    core::Tensor active_nb_addrs, active_nb_masks;
    std::tie(active_nb_addrs, active_nb_masks) =
            BufferRadiusNeighbors(active_addrs);

    // Input
    std::unordered_map<std::string, core::Tensor> srcs = {
            {"indices", active_addrs.To(core::Dtype::Int64)},
            {"nb_indices", active_nb_addrs.To(core::Dtype::Int64)},
            {"nb_masks", active_nb_masks},
            {"block_keys",
             core::Hashmap::ReinterpretBufferTensor(
                     block_hashmap_->GetKeyTensor(),
                     {block_hashmap_->GetCapacity(), 3}, core::Dtype::Int64)},
            {"block_values",
             core::Hashmap::ReinterpretBufferTensor(
                     block_hashmap_->GetValueTensor(),
                     {block_hashmap_->GetCapacity(), block_resolution_,
                      block_resolution_, block_resolution_, 2},
                     core::Dtype::Float32)},
            {"resolution", core::Tensor(std::vector<int64_t>{block_resolution_},
                                        {}, core::Dtype::Int64, device_)},
            {"voxel_size", core::Tensor(std::vector<float>{voxel_size_}, {},
                                        core::Dtype::Float32, device_)}};

    std::unordered_map<std::string, core::Tensor> dsts;
    core::kernel::GeneralEW(
            srcs, dsts, core::kernel::GeneralEWOpCode::TSDFSurfaceExtraction);
    if (dsts.count("points") == 0) {
        utility::LogError(
                "[TSDFVoxelGrid] extract surface launch failed, points "
                "expected "
                "to return.");
    }
    return PointCloud(core::TensorList::FromTensor(dsts.at("points")));
}

std::pair<core::Tensor, core::Tensor> TSDFVoxelGrid::BufferRadiusNeighbors(
        const core::Tensor &active_addrs) {
    core::Tensor key_buffer_int3_tensor =
            block_hashmap_->ReinterpretBufferTensor(
                    block_hashmap_->GetKeyTensor(),
                    {block_hashmap_->GetCapacity(), 3}, core::Dtype::Int64);

    core::Tensor active_keys = key_buffer_int3_tensor.IndexGet(
            {active_addrs.To(core::Dtype::Int64)});
    int64_t n = active_keys.GetShape()[0];

    // Fill in radius nearest neighbors
    core::Tensor keys_nb({27, n, 3}, core::Dtype::Int64, device_);
    for (int nb = 0; nb < 27; ++nb) {
        int dz = nb / 9;
        int dy = (nb % 9) / 3;
        int dx = nb % 3;
        core::Tensor dt =
                core::Tensor(std::vector<int64_t>{dx - 1, dy - 1, dz - 1},
                             {1, 3}, core::Dtype::Int64, device_);
        keys_nb[nb] = active_keys + dt;
    }
    keys_nb = keys_nb.View({27 * n, 3});

    core::Tensor addrs_nb, masks_nb;
    block_hashmap_->Find(keys_nb, addrs_nb, masks_nb);
    return std::make_pair(addrs_nb, masks_nb);
}
}  // namespace geometry
}  // namespace t
}  // namespace open3d