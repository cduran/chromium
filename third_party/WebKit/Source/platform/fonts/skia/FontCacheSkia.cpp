
/*
 * Copyright (c) 2006, 2007, 2008, 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unicode/locid.h>

#include <memory>
#include <utility>

#include "SkFontMgr.h"
#include "SkStream.h"
#include "SkTypeface.h"

#include "build/build_config.h"
#include "platform/Language.h"
#include "platform/font_family_names.h"
#include "platform/fonts/AlternateFontFamily.h"
#include "platform/fonts/BitmapGlyphsBlacklist.h"
#include "platform/fonts/FontCache.h"
#include "platform/fonts/FontDescription.h"
#include "platform/fonts/FontFaceCreationParams.h"
#include "platform/fonts/SimpleFontData.h"
#include "platform/graphics/skia/SkiaUtils.h"
#include "platform/wtf/Assertions.h"
#include "platform/wtf/text/AtomicString.h"
#include "platform/wtf/text/CString.h"
#include "public/platform/Platform.h"
#include "public/platform/linux/WebSandboxSupport.h"

namespace blink {

AtomicString ToAtomicString(const SkString& str) {
  return AtomicString::FromUTF8(str.c_str(), str.size());
}

#if defined(OS_ANDROID) || defined(OS_LINUX) || defined(OS_FUCHSIA)
// Android special locale for retrieving the color emoji font
// based on the proposed changes in UTR #51 for introducing
// an Emoji script code:
// http://www.unicode.org/reports/tr51/proposed.html#Emoji_Script
static const char kAndroidColorEmojiLocale[] = "und-Zsye";

// This function is called on android or when we are emulating android fonts on
// linux and the embedder has overriden the default fontManager with
// WebFontRendering::setSkiaFontMgr.
// static
AtomicString FontCache::GetFamilyNameForCharacter(
    SkFontMgr* fm,
    UChar32 c,
    const FontDescription& font_description,
    FontFallbackPriority fallback_priority) {
  DCHECK(fm);

  const size_t kMaxLocales = 4;
  const char* bcp47_locales[kMaxLocales];
  size_t locale_count = 0;

  // Fill in the list of locales in the reverse priority order.
  // Skia expects the highest array index to be the first priority.
  const LayoutLocale* content_locale = font_description.Locale();
  if (const LayoutLocale* han_locale =
          LayoutLocale::LocaleForHan(content_locale))
    bcp47_locales[locale_count++] = han_locale->LocaleForHanForSkFontMgr();
  bcp47_locales[locale_count++] =
      LayoutLocale::GetDefault().LocaleForSkFontMgr();
  if (content_locale)
    bcp47_locales[locale_count++] = content_locale->LocaleForSkFontMgr();
  if (fallback_priority == FontFallbackPriority::kEmojiEmoji)
    bcp47_locales[locale_count++] = kAndroidColorEmojiLocale;
  SECURITY_DCHECK(locale_count <= kMaxLocales);
  sk_sp<SkTypeface> typeface(fm->matchFamilyStyleCharacter(
      nullptr, SkFontStyle(), bcp47_locales, locale_count, c));
  if (!typeface)
    return g_empty_atom;

  SkString skia_family_name;
  typeface->getFamilyName(&skia_family_name);
  return ToAtomicString(skia_family_name);
}
#endif  // defined(OS_ANDROID) || defined(OS_LINUX) || defined(OS_FUCHSIA)

void FontCache::PlatformInit() {}

scoped_refptr<SimpleFontData> FontCache::FallbackOnStandardFontStyle(
    const FontDescription& font_description,
    UChar32 character) {
  FontDescription substitute_description(font_description);
  substitute_description.SetStyle(NormalSlopeValue());
  substitute_description.SetWeight(NormalWeightValue());

  FontFaceCreationParams creation_params(
      substitute_description.Family().Family());
  FontPlatformData* substitute_platform_data =
      GetFontPlatformData(substitute_description, creation_params);
  if (substitute_platform_data &&
      substitute_platform_data->FontContainsCharacter(character)) {
    FontPlatformData platform_data =
        FontPlatformData(*substitute_platform_data);
    platform_data.SetSyntheticBold(font_description.Weight() >=
                                   BoldThreshold());
    platform_data.SetSyntheticItalic(font_description.Style() ==
                                     ItalicSlopeValue());
    return FontDataFromFontPlatformData(&platform_data, kDoNotRetain);
  }

  return nullptr;
}

scoped_refptr<SimpleFontData> FontCache::GetLastResortFallbackFont(
    const FontDescription& description,
    ShouldRetain should_retain) {
  const FontFaceCreationParams fallback_creation_params(
      GetFallbackFontFamily(description));
  const FontPlatformData* font_platform_data = GetFontPlatformData(
      description, fallback_creation_params, AlternateFontName::kLastResort);

  // We should at least have Sans or Arial which is the last resort fallback of
  // SkFontHost ports.
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    sans_creation_params,
                                    (FontFamilyNames::Sans));
    font_platform_data = GetFontPlatformData(description, sans_creation_params,
                                             AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    arial_creation_params,
                                    (FontFamilyNames::Arial));
    font_platform_data = GetFontPlatformData(description, arial_creation_params,
                                             AlternateFontName::kLastResort);
  }
#if defined(OS_WIN)
  // Try some more Windows-specific fallbacks.
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    msuigothic_creation_params,
                                    (FontFamilyNames::MS_UI_Gothic));
    font_platform_data =
        GetFontPlatformData(description, msuigothic_creation_params,
                            AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    mssansserif_creation_params,
                                    (FontFamilyNames::Microsoft_Sans_Serif));
    font_platform_data =
        GetFontPlatformData(description, mssansserif_creation_params,
                            AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    segoeui_creation_params,
                                    (FontFamilyNames::Segoe_UI));
    font_platform_data = GetFontPlatformData(
        description, segoeui_creation_params, AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    calibri_creation_params,
                                    (FontFamilyNames::Calibri));
    font_platform_data = GetFontPlatformData(
        description, calibri_creation_params, AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    timesnewroman_creation_params,
                                    (FontFamilyNames::Times_New_Roman));
    font_platform_data =
        GetFontPlatformData(description, timesnewroman_creation_params,
                            AlternateFontName::kLastResort);
  }
  if (!font_platform_data) {
    DEFINE_THREAD_SAFE_STATIC_LOCAL(const FontFaceCreationParams,
                                    couriernew_creation_params,
                                    (FontFamilyNames::Courier_New));
    font_platform_data =
        GetFontPlatformData(description, couriernew_creation_params,
                            AlternateFontName::kLastResort);
  }
#endif

  DCHECK(font_platform_data);
  return FontDataFromFontPlatformData(font_platform_data, should_retain);
}

PaintTypeface FontCache::CreateTypeface(
    const FontDescription& font_description,
    const FontFaceCreationParams& creation_params,
    CString& name) {
#if !defined(OS_WIN) && !defined(OS_ANDROID) && !defined(OS_FUCHSIA)
  // TODO(fuchsia): Revisit this and other font code for Fuchsia.

  if (creation_params.CreationType() == kCreateFontByFciIdAndTtcIndex) {
    if (Platform::Current()->GetSandboxSupport()) {
      return PaintTypeface::FromFontConfigInterfaceIdAndTtcIndex(
          creation_params.FontconfigInterfaceId(), creation_params.TtcIndex());
    }
    return PaintTypeface::FromFilenameAndTtcIndex(
        creation_params.Filename().data(), creation_params.TtcIndex());
  }
#endif

  AtomicString family = creation_params.Family();
  DCHECK_NE(family, FontFamilyNames::system_ui);
  // If we're creating a fallback font (e.g. "-webkit-monospace"), convert the
  // name into the fallback name (like "monospace") that fontconfig understands.
  if (!family.length() || family.StartsWith("-webkit-")) {
    name = GetFallbackFontFamily(font_description).GetString().Utf8();
  } else {
    // convert the name to utf8
    name = family.Utf8();
  }

#if defined(OS_WIN)
  // TODO(vmpstr): Deal with paint typeface here.
  if (sideloaded_fonts_) {
    HashMap<String, sk_sp<SkTypeface>>::iterator sideloaded_font =
        sideloaded_fonts_->find(name.data());
    if (sideloaded_font != sideloaded_fonts_->end())
      return PaintTypeface::FromSkTypeface(sideloaded_font->value);
  }
#endif

#if defined(OS_LINUX) || defined(OS_WIN)
  // On linux if the fontManager has been overridden then we should be calling
  // the embedder provided font Manager rather than calling
  // SkTypeface::CreateFromName which may redirect the call to the default font
  // Manager.  On Windows the font manager is always present.
  if (font_manager_) {
    // TODO(vmpstr): Handle creating paint typefaces here directly. We need to
    // figure out whether it's safe to give |font_manager_| to PaintTypeface and
    // what that means on the GPU side.
    auto tf = sk_sp<SkTypeface>(font_manager_->matchFamilyStyle(
        name.data(), font_description.SkiaFontStyle()));
    return PaintTypeface::FromSkTypeface(std::move(tf));
  }
#endif

  // FIXME: Use m_fontManager, matchFamilyStyle instead of
  // legacyCreateTypeface on all platforms.
  return PaintTypeface::FromFamilyNameAndFontStyle(
      name.data(), font_description.SkiaFontStyle());
}

#if !defined(OS_WIN)
std::unique_ptr<FontPlatformData> FontCache::CreateFontPlatformData(
    const FontDescription& font_description,
    const FontFaceCreationParams& creation_params,
    float font_size,
    AlternateFontName) {
  CString name;
  PaintTypeface paint_tf =
      CreateTypeface(font_description, creation_params, name);
  if (!paint_tf)
    return nullptr;

  const auto& tf = paint_tf.ToSkTypeface();
  std::unique_ptr<FontPlatformData> font_platform_data =
      std::make_unique<FontPlatformData>(
          paint_tf, name, font_size,
          (font_description.Weight() >
               FontSelectionValue(200) +
                   FontSelectionValue(tf->fontStyle().weight()) ||
           font_description.IsSyntheticBold()),
          ((font_description.Style() == ItalicSlopeValue()) &&
           !tf->isItalic()) ||
              font_description.IsSyntheticItalic(),
          font_description.Orientation());

  font_platform_data->SetAvoidEmbeddedBitmaps(
      BitmapGlyphsBlacklist::AvoidEmbeddedBitmapsForTypeface(tf.get()));

  return font_platform_data;
}
#endif  // !defined(OS_WIN)

}  // namespace blink
