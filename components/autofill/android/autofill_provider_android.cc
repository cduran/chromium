// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/android/autofill_provider_android.h"

#include <memory>

#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/memory/ptr_util.h"
#include "components/autofill/android/form_data_android.h"
#include "components/autofill/core/browser/autofill_handler_proxy.h"
#include "components/autofill/core/common/autofill_constants.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "jni/AutofillProvider_jni.h"
#include "ui/gfx/geometry/rect_f.h"

using base::android::AttachCurrentThread;
using base::android::ConvertUTF16ToJavaString;
using base::android::ConvertUTF8ToJavaString;
using base::android::JavaRef;
using base::android::ScopedJavaLocalRef;
using content::BrowserThread;
using content::WebContents;
using gfx::RectF;

namespace autofill {

AutofillProviderAndroid::AutofillProviderAndroid(
    const JavaRef<jobject>& jcaller,
    content::WebContents* web_contents)
    : id_(kNoQueryId), web_contents_(web_contents), check_submission_(false) {
  JNIEnv* env = AttachCurrentThread();
  java_ref_ = JavaObjectWeakGlobalRef(env, jcaller);
  Java_AutofillProvider_setNativeAutofillProvider(
      env, jcaller, reinterpret_cast<jlong>(this));
}

AutofillProviderAndroid::~AutofillProviderAndroid() {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  // Remove the reference to this object on the Java side.
  Java_AutofillProvider_setNativeAutofillProvider(env, obj, 0);
}

void AutofillProviderAndroid::OnQueryFormFieldAutofill(
    AutofillHandlerProxy* handler,
    int32_t id,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  // The id isn't passed to Java side because Android API guarantees the
  // response is always for current session, so we just use the current id
  // in response, see OnAutofillAvailable.
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  id_ = id;

  // Focus or field value change will also trigger the query, so it should be
  // ignored if the form is same.
  if (ShouldStartNewSession(handler, form))
    StartNewSession(handler, form, field, bounding_box);
}

bool AutofillProviderAndroid::ShouldStartNewSession(
    AutofillHandlerProxy* handler,
    const FormData& form) {
  // Only start a new session when form or handler is changed, the change of
  // handler indicates query from other frame and a new session is needed.
  return !IsCurrentlyLinkedForm(form) || !IsCurrentlyLinkedHandler(handler);
}

void AutofillProviderAndroid::StartNewSession(AutofillHandlerProxy* handler,
                                              const FormData& form,
                                              const FormFieldData& field,
                                              const gfx::RectF& bounding_box) {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  form_ = std::make_unique<FormDataAndroid>(form);

  size_t index;
  if (!form_->GetFieldIndex(field, &index))
    return;

  gfx::RectF transformed_bounding = ToClientAreaBound(bounding_box);

  ScopedJavaLocalRef<jobject> form_obj = form_->GetJavaPeer();
  handler_ = handler->GetWeakPtr();
  Java_AutofillProvider_startAutofillSession(
      env, obj, form_obj, index, transformed_bounding.x(),
      transformed_bounding.y(), transformed_bounding.width(),
      transformed_bounding.height());
}

void AutofillProviderAndroid::OnAutofillAvailable(JNIEnv* env,
                                                  jobject jcaller,
                                                  jobject formData) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (handler_) {
    const FormData& form = form_->GetAutofillValues();
    SendFormDataToRenderer(handler_.get(), id_, form);
  }
}

void AutofillProviderAndroid::OnTextFieldDidChange(
    AutofillHandlerProxy* handler,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box,
    const base::TimeTicks timestamp) {
  FireFormFieldDidChanged(handler, form, field, bounding_box);
}

void AutofillProviderAndroid::OnTextFieldDidScroll(
    AutofillHandlerProxy* handler,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  size_t index;
  if (!IsCurrentlyLinkedHandler(handler) || !IsCurrentlyLinkedForm(form) ||
      !form_->GetSimilarFieldIndex(field, &index))
    return;

  form_->OnFormFieldDidChange(index, field.value);
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  gfx::RectF transformed_bounding = ToClientAreaBound(bounding_box);
  Java_AutofillProvider_onTextFieldDidScroll(
      env, obj, index, transformed_bounding.x(), transformed_bounding.y(),
      transformed_bounding.width(), transformed_bounding.height());
}

void AutofillProviderAndroid::OnSelectControlDidChange(
    AutofillHandlerProxy* handler,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  if (ShouldStartNewSession(handler, form))
    StartNewSession(handler, form, field, bounding_box);
  FireFormFieldDidChanged(handler, form, field, bounding_box);
}

void AutofillProviderAndroid::FireSuccessfulSubmission(
    SubmissionSource source) {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;
  Java_AutofillProvider_onFormSubmitted(env, obj, (int)source);
  Reset();
}

bool AutofillProviderAndroid::OnFormSubmitted(AutofillHandlerProxy* handler,
                                              const FormData& form,
                                              bool known_success,
                                              SubmissionSource source,
                                              base::TimeTicks timestamp) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!IsCurrentlyLinkedHandler(handler) || !IsCurrentlyLinkedForm(form))
    return false;

  if (known_success || source == SubmissionSource::FORM_SUBMISSION) {
    FireSuccessfulSubmission(source);
  } else {
    check_submission_ = true;
    pending_submission_source_ = source;
  }
  return true;
}

void AutofillProviderAndroid::OnFocusNoLongerOnForm(
    AutofillHandlerProxy* handler) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!IsCurrentlyLinkedHandler(handler))
    return;

  OnFocusChanged(false, 0, RectF());
}

void AutofillProviderAndroid::OnFocusOnFormField(
    AutofillHandlerProxy* handler,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  size_t index;
  if (!IsCurrentlyLinkedHandler(handler) || !IsCurrentlyLinkedForm(form) ||
      !form_->GetSimilarFieldIndex(field, &index))
    return;

  // Because this will trigger a suggestion query, set request id to browser
  // initiated request.
  id_ = kNoQueryId;

  OnFocusChanged(true, index, ToClientAreaBound(bounding_box));
}

void AutofillProviderAndroid::OnFocusChanged(bool focus_on_form,
                                             size_t index,
                                             const gfx::RectF& bounding_box) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  Java_AutofillProvider_onFocusChanged(
      env, obj, focus_on_form, index, bounding_box.x(), bounding_box.y(),
      bounding_box.width(), bounding_box.height());
}

void AutofillProviderAndroid::FireFormFieldDidChanged(
    AutofillHandlerProxy* handler,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  size_t index;
  if (!IsCurrentlyLinkedHandler(handler) || !IsCurrentlyLinkedForm(form) ||
      !form_->GetSimilarFieldIndex(field, &index))
    return;

  form_->OnFormFieldDidChange(index, field.value);
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  gfx::RectF transformed_bounding = ToClientAreaBound(bounding_box);
  Java_AutofillProvider_onFormFieldDidChange(
      env, obj, index, transformed_bounding.x(), transformed_bounding.y(),
      transformed_bounding.width(), transformed_bounding.height());
}

void AutofillProviderAndroid::OnDidFillAutofillFormData(
    AutofillHandlerProxy* handler,
    const FormData& form,
    base::TimeTicks timestamp) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (handler != handler_.get() || !IsCurrentlyLinkedForm(form))
    return;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  Java_AutofillProvider_onDidFillAutofillFormData(env, obj);
}

void AutofillProviderAndroid::OnFormsSeen(AutofillHandlerProxy* handler,
                                          const std::vector<FormData>& forms,
                                          const base::TimeTicks) {
  handler_for_testing_ = handler->GetWeakPtr();
  if (!check_submission_)
    return;

  if (handler != handler_.get())
    return;

  if (form_.get() == nullptr)
    return;

  for (auto const& form : forms) {
    if (form_->SimilarFormAs(form))
      return;
  }
  // The form_ disappeared after it was submitted, we consider the submission
  // succeeded.
  FireSuccessfulSubmission(pending_submission_source_);
}

void AutofillProviderAndroid::Reset(AutofillHandlerProxy* handler) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (handler == handler_.get()) {
    JNIEnv* env = AttachCurrentThread();
    ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
    if (obj.is_null())
      return;

    Java_AutofillProvider_reset(env, obj);
  }
}

bool AutofillProviderAndroid::IsCurrentlyLinkedHandler(
    AutofillHandlerProxy* handler) {
  return handler == handler_.get();
}

bool AutofillProviderAndroid::IsCurrentlyLinkedForm(const FormData& form) {
  return form_ && form_->SimilarFormAs(form);
}

gfx::RectF AutofillProviderAndroid::ToClientAreaBound(
    const gfx::RectF& bounding_box) {
  gfx::Rect client_area = web_contents_->GetContainerBounds();
  return bounding_box + client_area.OffsetFromOrigin();
}

void AutofillProviderAndroid::Reset() {
  form_.reset(nullptr);
  id_ = kNoQueryId;
  check_submission_ = false;
}

void AutofillProviderAndroid::FireSelectControlDidChangeForTesting(
    JNIEnv* env,
    jobject jcaller,
    jint index,
    jstring id,
    jobjectArray options,
    jint selected_option) {
  FormData form_data;
  FormFieldData form_field_data;
  AutofillHandlerProxy* handler = nullptr;
  if (form_.get() == nullptr) {
    // Build a fake form
    for (int i = 0; i < index; i++)
      form_data.fields.push_back(FormFieldData());

    base::android::AppendJavaStringArrayToStringVector(
        env, options, &(form_field_data.option_values));
    form_field_data.option_contents = form_field_data.option_values;
    form_field_data.id = base::android::ConvertJavaStringToUTF16(env, id);
    form_data.fields.push_back(form_field_data);
    handler = handler_for_testing_.get();
  } else {
    form_data = form_->form_for_testing();
    form_field_data = form_data.fields[index];
    handler = handler_.get();
  }
  DCHECK(form_field_data.option_values.size() != 0);
  DCHECK(!handler);
  form_field_data.value = base::android::ConvertJavaStringToUTF16(
      env, static_cast<jstring>(
               env->GetObjectArrayElement(options, selected_option)));
  OnSelectControlDidChange(handler, form_data, form_field_data, gfx::RectF());
}

}  // namespace autofill
