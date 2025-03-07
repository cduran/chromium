// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_API_WEBRTC_LOGGING_PRIVATE_WEBRTC_LOGGING_PRIVATE_API_H_
#define CHROME_BROWSER_EXTENSIONS_API_WEBRTC_LOGGING_PRIVATE_WEBRTC_LOGGING_PRIVATE_API_H_

#include <string>

#include "chrome/browser/extensions/chrome_extension_function.h"
#include "chrome/common/extensions/api/webrtc_logging_private.h"
#include "media/media_features.h"

#if BUILDFLAG(ENABLE_WEBRTC)
#include "chrome/browser/media/webrtc/audio_debug_recordings_handler.h"
#include "chrome/browser/media/webrtc/webrtc_logging_handler_host.h"
#endif

namespace content {

class RenderProcessHost;

}

namespace extensions {

class WebrtcLoggingPrivateFunction : public ChromeAsyncExtensionFunction {
 protected:
  ~WebrtcLoggingPrivateFunction() override {}

#if BUILDFLAG(ENABLE_WEBRTC)
  // Returns the RenderProcessHost associated with the given |request|
  // authorized by the |security_origin|. Returns null if unauthorized or
  // the RPH does not exist.
  content::RenderProcessHost* RphFromRequest(
      const api::webrtc_logging_private::RequestInfo& request,
      const std::string& security_origin);

  scoped_refptr<WebRtcLoggingHandlerHost> LoggingHandlerFromRequest(
      const api::webrtc_logging_private::RequestInfo& request,
      const std::string& security_origin);
#endif
};

class WebrtcLoggingPrivateFunctionWithGenericCallback
    : public WebrtcLoggingPrivateFunction {
 protected:
  ~WebrtcLoggingPrivateFunctionWithGenericCallback() override {}

#if BUILDFLAG(ENABLE_WEBRTC)
  // Finds the appropriate logging handler for performing the task and prepares
  // a generic callback object for when the task is completed.
  // If the logging handler can't be found for the given request+origin, the
  // returned ptr will be null.
  scoped_refptr<WebRtcLoggingHandlerHost> PrepareTask(
    const api::webrtc_logging_private::RequestInfo& request,
    const std::string& security_origin,
    WebRtcLoggingHandlerHost::GenericDoneCallback* callback);

  // Must be called on UI thread.
  void FireCallback(bool success, const std::string& error_message);
#endif
};

class WebrtcLoggingPrivateFunctionWithUploadCallback
    : public WebrtcLoggingPrivateFunction {
 protected:
  ~WebrtcLoggingPrivateFunctionWithUploadCallback() override {}

#if BUILDFLAG(ENABLE_WEBRTC)
  // Must be called on UI thread.
  void FireCallback(bool success, const std::string& report_id,
                    const std::string& error_message);
#endif
};

class WebrtcLoggingPrivateFunctionWithRecordingDoneCallback
    : public WebrtcLoggingPrivateFunction {
 protected:
  ~WebrtcLoggingPrivateFunctionWithRecordingDoneCallback() override {}

#if BUILDFLAG(ENABLE_WEBRTC)
  // Must be called on UI thread.
  void FireErrorCallback(const std::string& error_message);
  void FireCallback(const std::string& prefix_path,
                    bool did_stop,
                    bool did_manual_stop);
#endif
};

class WebrtcLoggingPrivateSetMetaDataFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.setMetaData",
                             WEBRTCLOGGINGPRIVATE_SETMETADATA)
  WebrtcLoggingPrivateSetMetaDataFunction() {}

 private:
  ~WebrtcLoggingPrivateSetMetaDataFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStartFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.start",
                             WEBRTCLOGGINGPRIVATE_START)
  WebrtcLoggingPrivateStartFunction() {}

 private:
  ~WebrtcLoggingPrivateStartFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateSetUploadOnRenderCloseFunction
    : public WebrtcLoggingPrivateFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.setUploadOnRenderClose",
                             WEBRTCLOGGINGPRIVATE_SETUPLOADONRENDERCLOSE)
  WebrtcLoggingPrivateSetUploadOnRenderCloseFunction() {}

 private:
  ~WebrtcLoggingPrivateSetUploadOnRenderCloseFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStopFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.stop",
                             WEBRTCLOGGINGPRIVATE_STOP)
  WebrtcLoggingPrivateStopFunction() {}

 private:
  ~WebrtcLoggingPrivateStopFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStoreFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.store",
                             WEBRTCLOGGINGPRIVATE_STORE)
  WebrtcLoggingPrivateStoreFunction() {}

 private:
  ~WebrtcLoggingPrivateStoreFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateUploadStoredFunction
    : public WebrtcLoggingPrivateFunctionWithUploadCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.uploadStored",
                             WEBRTCLOGGINGPRIVATE_UPLOADSTORED)
  WebrtcLoggingPrivateUploadStoredFunction() {}

 private:
  ~WebrtcLoggingPrivateUploadStoredFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateUploadFunction
    : public WebrtcLoggingPrivateFunctionWithUploadCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.upload",
                             WEBRTCLOGGINGPRIVATE_UPLOAD)
  WebrtcLoggingPrivateUploadFunction() {}

 private:
  ~WebrtcLoggingPrivateUploadFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateDiscardFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.discard",
                             WEBRTCLOGGINGPRIVATE_DISCARD)
  WebrtcLoggingPrivateDiscardFunction() {}

 private:
  ~WebrtcLoggingPrivateDiscardFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStartRtpDumpFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.startRtpDump",
                             WEBRTCLOGGINGPRIVATE_STARTRTPDUMP)
  WebrtcLoggingPrivateStartRtpDumpFunction() {}

 private:
  ~WebrtcLoggingPrivateStartRtpDumpFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStopRtpDumpFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.stopRtpDump",
                             WEBRTCLOGGINGPRIVATE_STOPRTPDUMP)
  WebrtcLoggingPrivateStopRtpDumpFunction() {}

 private:
  ~WebrtcLoggingPrivateStopRtpDumpFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStartAudioDebugRecordingsFunction
    : public WebrtcLoggingPrivateFunctionWithRecordingDoneCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.startAudioDebugRecordings",
                             WEBRTCLOGGINGPRIVATE_STARTAUDIODEBUGRECORDINGS)
  WebrtcLoggingPrivateStartAudioDebugRecordingsFunction() {}

 private:
  ~WebrtcLoggingPrivateStartAudioDebugRecordingsFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStopAudioDebugRecordingsFunction
    : public WebrtcLoggingPrivateFunctionWithRecordingDoneCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.stopAudioDebugRecordings",
                             WEBRTCLOGGINGPRIVATE_STOPAUDIODEBUGRECORDINGS)
  WebrtcLoggingPrivateStopAudioDebugRecordingsFunction() {}

 private:
  ~WebrtcLoggingPrivateStopAudioDebugRecordingsFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateStartEventLoggingFunction
    : public WebrtcLoggingPrivateFunctionWithGenericCallback {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.startEventLogging",
                             WEBRTCLOGGINGPRIVATE_STARTEVENTLOGGING)
  WebrtcLoggingPrivateStartEventLoggingFunction() {}

 private:
  ~WebrtcLoggingPrivateStartEventLoggingFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;
};

class WebrtcLoggingPrivateGetLogsDirectoryFunction
    : public WebrtcLoggingPrivateFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("webrtcLoggingPrivate.getLogsDirectory",
                             WEBRTCLOGGINGPRIVATE_GETLOGSDIRECTORY)
  WebrtcLoggingPrivateGetLogsDirectoryFunction() {}

 private:
  ~WebrtcLoggingPrivateGetLogsDirectoryFunction() override {}

  // ExtensionFunction overrides.
  bool RunAsync() override;

#if BUILDFLAG(ENABLE_WEBRTC)
  // Must be called on UI thread.
  void FireErrorCallback(const std::string& error_message);
  void FireCallback(const std::string& filesystem_id,
                    const std::string& base_name);
#endif
};

}  // namespace extensions

#endif  // CHROME_BROWSER_EXTENSIONS_API_WEBRTC_LOGGING_PRIVATE_WEBRTC_LOGGING_PRIVATE_API_H_
