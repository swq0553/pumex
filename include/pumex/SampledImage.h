//
// Copyright(c) 2017-2018 Paweł Księżopolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once
#include <unordered_map>
#include <pumex/Export.h>
#include <pumex/Resource.h>

namespace pumex
{

class ImageView;

class PUMEX_EXPORT SampledImage : public Resource
{
public:
  SampledImage()                               = delete;
  SampledImage(std::shared_ptr<ImageView> imageView);
  SampledImage(const std::string& resourceName);
  SampledImage(const SampledImage&)            = delete;
  SampledImage& operator=(const SampledImage&) = delete;
  SampledImage(SampledImage&&)                 = delete;
  SampledImage& operator=(SampledImage&&)      = delete;
  virtual ~SampledImage();

  std::pair<bool, VkDescriptorType> getDefaultDescriptorType() override;
  void                              validate(const RenderContext& renderContext) override;
  DescriptorValue                   getDescriptorValue(const RenderContext& renderContext) override;

protected:
  std::shared_ptr<ImageView> imageView;
  std::string                resourceName;
  bool                       registered = false;
};
	
}