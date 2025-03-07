// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/font_render_params.h"

#include <fontconfig/fontconfig.h>

#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/test/fontconfig_util_linux.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/font.h"
#include "ui/gfx/linux_font_delegate.h"

namespace gfx {

namespace {

// Strings appearing at the beginning and end of Fontconfig XML files.
const char kFontconfigFileHeader[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
    "<fontconfig>\n";
const char kFontconfigFileFooter[] = "</fontconfig>";

// Strings appearing at the beginning and end of Fontconfig <match> stanzas.
const char kFontconfigMatchFontHeader[] = "  <match target=\"font\">\n";
const char kFontconfigMatchPatternHeader[] = "  <match target=\"pattern\">\n";
const char kFontconfigMatchFooter[] = "  </match>\n";

// Implementation of LinuxFontDelegate that returns a canned FontRenderParams
// struct. This is used to isolate tests from the system's local configuration.
class TestFontDelegate : public LinuxFontDelegate {
 public:
  TestFontDelegate() {}
  ~TestFontDelegate() override {}

  void set_params(const FontRenderParams& params) { params_ = params; }

  FontRenderParams GetDefaultFontRenderParams() const override {
    return params_;
  }
  void GetDefaultFontDescription(std::string* family_out,
                                 int* size_pixels_out,
                                 int* style_out,
                                 Font::Weight* weight_out,
                                 FontRenderParams* params_out) const override {
    NOTIMPLEMENTED();
  }

 private:
  FontRenderParams params_;

  DISALLOW_COPY_AND_ASSIGN(TestFontDelegate);
};

}  // namespace

class FontRenderParamsTest : public testing::Test {
 public:
  FontRenderParamsTest() {
    CHECK(temp_dir_.CreateUniqueTempDir());
    original_font_delegate_ = LinuxFontDelegate::instance();
    LinuxFontDelegate::SetInstance(&test_font_delegate_);
    ClearFontRenderParamsCacheForTest();
  }

  ~FontRenderParamsTest() override {
    LinuxFontDelegate::SetInstance(
        const_cast<LinuxFontDelegate*>(original_font_delegate_));
  }

  void SetUp() override {
    // Fontconfig should already be set up by the test runner.
    DCHECK(FcConfigGetCurrent());
  }

  void TearDown() override {
    // We might have made a mess introducing new fontconfig settings.  Reset the
    // state of fontconfig for the next test.
    base::SetUpFontconfig();
  }

 protected:
  base::ScopedTempDir temp_dir_;
  const LinuxFontDelegate* original_font_delegate_;
  TestFontDelegate test_font_delegate_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FontRenderParamsTest);
};

TEST_F(FontRenderParamsTest, Default) {
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) +
          // Specify the desired defaults via a font match rather than a pattern
          // match (since this is the style generally used in
          // /etc/fonts/conf.d).
          kFontconfigMatchFontHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          base::CreateFontconfigEditStanza("autohint", "bool", "true") +
          base::CreateFontconfigEditStanza("hinting", "bool", "true") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintslight") +
          base::CreateFontconfigEditStanza("rgba", "const", "rgb") +
          kFontconfigMatchFooter +
          // Add a font match for Arial. Since it specifies a family, it
          // shouldn't take effect when querying default settings.
          kFontconfigMatchFontHeader +
          base::CreateFontconfigTestStanza("family", "eq", "string", "Arial") +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          base::CreateFontconfigEditStanza("autohint", "bool", "false") +
          base::CreateFontconfigEditStanza("hinting", "bool", "true") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintfull") +
          base::CreateFontconfigEditStanza("rgba", "const", "none") +
          kFontconfigMatchFooter +
          // Add font matches for fonts between 10 and 20 points or pixels.
          // Since they specify sizes, they also should not affect the defaults.
          kFontconfigMatchFontHeader +
          base::CreateFontconfigTestStanza("size", "more_eq", "double",
                                           "10.0") +
          base::CreateFontconfigTestStanza("size", "less_eq", "double",
                                           "20.0") +
          base::CreateFontconfigEditStanza("antialias", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigMatchFontHeader +
          base::CreateFontconfigTestStanza("pixel_size", "more_eq", "double",
                                           "10.0") +
          base::CreateFontconfigTestStanza("pixel_size", "less_eq", "double",
                                           "20.0") +
          base::CreateFontconfigEditStanza("antialias", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  FontRenderParams params = GetFontRenderParams(
      FontRenderParamsQuery(), NULL);
  EXPECT_TRUE(params.antialiasing);
  EXPECT_TRUE(params.autohinter);
  EXPECT_TRUE(params.use_bitmaps);
  EXPECT_EQ(FontRenderParams::HINTING_SLIGHT, params.hinting);
  EXPECT_FALSE(params.subpixel_positioning);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_RGB,
            params.subpixel_rendering);
}

TEST_F(FontRenderParamsTest, Size) {
  ASSERT_TRUE(base::LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          base::CreateFontconfigEditStanza("hinting", "bool", "true") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintfull") +
          base::CreateFontconfigEditStanza("rgba", "const", "none") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("pixelsize", "less_eq", "double",
                                           "10") +
          base::CreateFontconfigEditStanza("antialias", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("size", "more_eq", "double", "20") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintslight") +
          base::CreateFontconfigEditStanza("rgba", "const", "rgb") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  // The defaults should be used when the supplied size isn't matched by the
  // second or third blocks.
  FontRenderParamsQuery query;
  query.pixel_size = 12;
  FontRenderParams params = GetFontRenderParams(query, NULL);
  EXPECT_TRUE(params.antialiasing);
  EXPECT_EQ(FontRenderParams::HINTING_FULL, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_NONE,
            params.subpixel_rendering);

  query.pixel_size = 10;
  params = GetFontRenderParams(query, NULL);
  EXPECT_FALSE(params.antialiasing);
  EXPECT_EQ(FontRenderParams::HINTING_FULL, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_NONE,
            params.subpixel_rendering);

  query.pixel_size = 0;
  query.point_size = 20;
  params = GetFontRenderParams(query, NULL);
  EXPECT_TRUE(params.antialiasing);
  EXPECT_EQ(FontRenderParams::HINTING_SLIGHT, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_RGB,
            params.subpixel_rendering);
}

TEST_F(FontRenderParamsTest, Style) {
  // Load a config that disables subpixel rendering for bold text and disables
  // hinting for italic text.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          base::CreateFontconfigEditStanza("hinting", "bool", "true") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintslight") +
          base::CreateFontconfigEditStanza("rgba", "const", "rgb") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("weight", "eq", "const", "bold") +
          base::CreateFontconfigEditStanza("rgba", "const", "none") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("slant", "eq", "const", "italic") +
          base::CreateFontconfigEditStanza("hinting", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  FontRenderParamsQuery query;
  query.style = Font::NORMAL;
  FontRenderParams params = GetFontRenderParams(query, NULL);
  EXPECT_EQ(FontRenderParams::HINTING_SLIGHT, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_RGB,
            params.subpixel_rendering);

  query.weight = Font::Weight::BOLD;
  params = GetFontRenderParams(query, NULL);
  EXPECT_EQ(FontRenderParams::HINTING_SLIGHT, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_NONE,
            params.subpixel_rendering);

  query.weight = Font::Weight::NORMAL;
  query.style = Font::ITALIC;
  params = GetFontRenderParams(query, NULL);
  EXPECT_EQ(FontRenderParams::HINTING_NONE, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_RGB,
            params.subpixel_rendering);

  query.weight = Font::Weight::BOLD;
  query.style = Font::ITALIC;
  params = GetFontRenderParams(query, NULL);
  EXPECT_EQ(FontRenderParams::HINTING_NONE, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_NONE,
            params.subpixel_rendering);
}

TEST_F(FontRenderParamsTest, Scalable) {
  // Load a config that only enables antialiasing for scalable fonts.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("scalable", "eq", "bool", "true") +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  // Check that we specifically ask how scalable fonts should be rendered.
  FontRenderParams params = GetFontRenderParams(
      FontRenderParamsQuery(), NULL);
  EXPECT_TRUE(params.antialiasing);
}

TEST_F(FontRenderParamsTest, UseBitmaps) {
  // Load a config that enables embedded bitmaps for fonts <= 10 pixels.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("embeddedbitmap", "bool", "false") +
          kFontconfigMatchFooter + kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("pixelsize", "less_eq", "double",
                                           "10") +
          base::CreateFontconfigEditStanza("embeddedbitmap", "bool", "true") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  FontRenderParamsQuery query;
  FontRenderParams params = GetFontRenderParams(query, NULL);
  EXPECT_FALSE(params.use_bitmaps);

  query.pixel_size = 5;
  params = GetFontRenderParams(query, NULL);
  EXPECT_TRUE(params.use_bitmaps);
}

TEST_F(FontRenderParamsTest, ForceFullHintingWhenAntialiasingIsDisabled) {
  // Load a config that disables antialiasing and hinting while requesting
  // subpixel rendering.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "false") +
          base::CreateFontconfigEditStanza("hinting", "bool", "false") +
          base::CreateFontconfigEditStanza("hintstyle", "const", "hintnone") +
          base::CreateFontconfigEditStanza("rgba", "const", "rgb") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  // Full hinting should be forced. See the comment in GetFontRenderParams() for
  // more information.
  FontRenderParams params = GetFontRenderParams(
      FontRenderParamsQuery(), NULL);
  EXPECT_FALSE(params.antialiasing);
  EXPECT_EQ(FontRenderParams::HINTING_FULL, params.hinting);
  EXPECT_EQ(FontRenderParams::SUBPIXEL_RENDERING_NONE,
            params.subpixel_rendering);
  EXPECT_FALSE(params.subpixel_positioning);
}

TEST_F(FontRenderParamsTest, ForceSubpixelPositioning) {
  {
    FontRenderParams params =
        GetFontRenderParams(FontRenderParamsQuery(), NULL);
    EXPECT_TRUE(params.antialiasing);
    EXPECT_FALSE(params.subpixel_positioning);
    SetFontRenderParamsDeviceScaleFactor(1.0f);
  }
  ClearFontRenderParamsCacheForTest();
  SetFontRenderParamsDeviceScaleFactor(1.25f);
  // Subpixel positioning should be forced.
  {
    FontRenderParams params =
        GetFontRenderParams(FontRenderParamsQuery(), NULL);
    EXPECT_TRUE(params.antialiasing);
    EXPECT_TRUE(params.subpixel_positioning);
    SetFontRenderParamsDeviceScaleFactor(1.0f);
  }
}

TEST_F(FontRenderParamsTest, OnlySetConfiguredValues) {
  // Configure the LinuxFontDelegate (which queries GtkSettings on desktop
  // Linux) to request subpixel rendering.
  FontRenderParams system_params;
  system_params.subpixel_rendering = FontRenderParams::SUBPIXEL_RENDERING_RGB;
  test_font_delegate_.set_params(system_params);

  // Load a Fontconfig config that enables antialiasing but doesn't say anything
  // about subpixel rendering.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) + kFontconfigMatchPatternHeader +
          base::CreateFontconfigEditStanza("antialias", "bool", "true") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  // The subpixel rendering setting from the delegate should make it through.
  FontRenderParams params = GetFontRenderParams(
      FontRenderParamsQuery(), NULL);
  EXPECT_EQ(system_params.subpixel_rendering, params.subpixel_rendering);
}

TEST_F(FontRenderParamsTest, NoFontconfigMatch) {
  // A default configuration was set up globally.  Reset it to a blank config.
  FcConfigSetCurrent(FcConfigCreate());

  FontRenderParams system_params;
  system_params.antialiasing = true;
  system_params.hinting = FontRenderParams::HINTING_MEDIUM;
  system_params.subpixel_rendering = FontRenderParams::SUBPIXEL_RENDERING_RGB;
  test_font_delegate_.set_params(system_params);

  FontRenderParamsQuery query;
  query.families.push_back("Arial");
  query.families.push_back("Times New Roman");
  query.pixel_size = 10;
  std::string suggested_family;
  FontRenderParams params = GetFontRenderParams(query, &suggested_family);

  // The system params and the first requested family should be returned.
  EXPECT_EQ(system_params.antialiasing, params.antialiasing);
  EXPECT_EQ(system_params.hinting, params.hinting);
  EXPECT_EQ(system_params.subpixel_rendering, params.subpixel_rendering);
  EXPECT_EQ(query.families[0], suggested_family);
}

TEST_F(FontRenderParamsTest, MissingFamily) {
  // With Arial and Verdana installed, request (in order) Helvetica, Arial, and
  // Verdana and check that Arial is returned.
  FontRenderParamsQuery query;
  query.families.push_back("Helvetica");
  query.families.push_back("Arial");
  query.families.push_back("Verdana");
  std::string suggested_family;
  GetFontRenderParams(query, &suggested_family);
  EXPECT_EQ("Arial", suggested_family);
}

TEST_F(FontRenderParamsTest, SubstituteFamily) {
  // Configure Fontconfig to use Verdana for both Helvetica and Arial.
  ASSERT_TRUE(LoadConfigDataIntoFontconfig(
      temp_dir_.GetPath(),
      std::string(kFontconfigFileHeader) +
          base::CreateFontconfigAliasStanza("Helvetica", "Verdana") +
          kFontconfigMatchPatternHeader +
          base::CreateFontconfigTestStanza("family", "eq", "string", "Arial") +
          base::CreateFontconfigEditStanza("family", "string", "Verdana") +
          kFontconfigMatchFooter + kFontconfigFileFooter));

  FontRenderParamsQuery query;
  query.families.push_back("Helvetica");
  std::string suggested_family;
  GetFontRenderParams(query, &suggested_family);
  EXPECT_EQ("Verdana", suggested_family);

  query.families.clear();
  query.families.push_back("Arial");
  suggested_family.clear();
  GetFontRenderParams(query, &suggested_family);
  EXPECT_EQ("Verdana", suggested_family);
}

}  // namespace gfx
