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

#pragma once

#include "open3d/visualization/gui/Widget.h"

namespace open3d {
namespace visualization {

namespace rendering {
class VideoProvider;
}  // namespace rendering

namespace gui {

class VideoWidget : public Widget {
public:
    VideoWidget(std::shared_ptr<rendering::VideoProvider> video);
    ~VideoWidget();

    std::shared_ptr<rendering::VideoProvider> GetVideoProvider();
    void SetVideoProvider(std::shared_ptr<rendering::VideoProvider> video);

    bool GetIsPlaying() const;
    void SetIsPlaying(bool playing);

    Widget::DrawResult Tick(const TickEvent& e) override;

    DrawResult Draw(const DrawContext& context) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gui
} // namespace visualization
} // namespace open3d