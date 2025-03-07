// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AudioWorkletProcessorDefinition_h
#define AudioWorkletProcessorDefinition_h

#include "modules/ModulesExport.h"
#include "modules/webaudio/AudioParamDescriptor.h"
#include "platform/bindings/ScriptWrappable.h"
#include "platform/bindings/TraceWrapperV8Reference.h"
#include "platform/heap/Handle.h"
#include "platform/wtf/text/WTFString.h"
#include "v8/include/v8.h"

namespace blink {

// Represents a JavaScript class definition registered in the
// AudioWorkletGlobalScope. After the registration, a definition class contains
// the V8 representation of class components (constructor, process callback,
// prototypes and parameter descriptors).
//
// This is constructed and destroyed on a worker thread, and all methods also
// must be called on the worker thread.
class MODULES_EXPORT AudioWorkletProcessorDefinition final
    : public GarbageCollectedFinalized<AudioWorkletProcessorDefinition>,
      public TraceWrapperBase {
 public:
  static AudioWorkletProcessorDefinition* Create(
      v8::Isolate*,
      const String& name,
      v8::Local<v8::Object> constructor,
      v8::Local<v8::Function> process);

  virtual ~AudioWorkletProcessorDefinition();

  const String& GetName() const { return name_; }
  v8::Local<v8::Object> ConstructorLocal(v8::Isolate*);
  v8::Local<v8::Function> ProcessLocal(v8::Isolate*);
  void SetAudioParamDescriptors(const HeapVector<AudioParamDescriptor>&);
  const Vector<String> GetAudioParamDescriptorNames() const;
  const AudioParamDescriptor* GetAudioParamDescriptor(const String& key) const;

  // Flag for data synchronization of definition between
  // AudioWorkletMessagingProxy and AudioWorkletGlobalScope.
  bool IsSynchronized() const { return is_synchronized_; }
  void MarkAsSynchronized() { is_synchronized_ = true; }

  void Trace(blink::Visitor* visitor) {
    visitor->Trace(audio_param_descriptors_);
  };
  void TraceWrappers(const ScriptWrappableVisitor*) const override;
  const char* NameInHeapSnapshot() const override {
    return "AudioWorkletProcessorDefinition";
  }

 private:
  AudioWorkletProcessorDefinition(
      v8::Isolate*,
      const String& name,
      v8::Local<v8::Object> constructor,
      v8::Local<v8::Function> process);

  const String name_;
  bool is_synchronized_ = false;

  // The definition is per global scope. The active instance of
  // |AudioProcessorWorklet| should be passed into these to perform JS function.
  TraceWrapperV8Reference<v8::Object> constructor_;
  TraceWrapperV8Reference<v8::Function> process_;

  HeapVector<AudioParamDescriptor> audio_param_descriptors_;
};

}  // namespace blink

#endif  // AudioWorkletProcessorDefinition_h
