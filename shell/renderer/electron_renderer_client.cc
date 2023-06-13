// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/electron_renderer_client.h"

#include "base/command_line.h"
#include "base/containers/contains.h"
#include "content/public/renderer/render_frame.h"
#include "electron/buildflags/buildflags.h"
#include "net/http/http_request_headers.h"
#include "shell/common/api/electron_bindings.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/event_emitter_caller.h"
#include "shell/common/node_bindings.h"
#include "shell/common/node_includes.h"
#include "shell/common/options_switches.h"
#include "shell/renderer/electron_render_frame_observer.h"
#include "shell/renderer/web_worker_observer.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"  // nogncheck

namespace electron {

ElectronRendererClient::ElectronRendererClient()
    : node_bindings_(
          NodeBindings::Create(NodeBindings::BrowserEnvironment::kRenderer)),
      electron_bindings_(
          std::make_unique<ElectronBindings>(node_bindings_->uv_loop())) {}

ElectronRendererClient::~ElectronRendererClient() {
  if (!env_)
    return;

  // Destroying the node environment will also run the uv loop,
  // Node.js expects `kExplicit` microtasks policy and will run microtasks
  // checkpoints after every call into JavaScript. Since we use a different
  // policy in the renderer - switch to `kExplicit` and then drop back to the
  // previous policy value.
  v8::Local<v8::Context> context = env_->context();
  v8::MicrotaskQueue* microtask_queue = context->GetMicrotaskQueue();
  auto old_policy = microtask_queue->microtasks_policy();
  DCHECK_EQ(microtask_queue->GetMicrotasksScopeDepth(), 0);
  microtask_queue->set_microtasks_policy(v8::MicrotasksPolicy::kExplicit);

  node::FreeEnvironment(env_);
  node::FreeIsolateData(node_bindings_->isolate_data());
  node_bindings_->set_isolate_data(nullptr);

  microtask_queue->set_microtasks_policy(old_policy);

  // ElectronBindings is tracking node environments.
  electron_bindings_->EnvironmentDestroyed(env_);
}

void ElectronRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  new ElectronRenderFrameObserver(render_frame, this);
  RendererClientBase::RenderFrameCreated(render_frame);
}

void ElectronRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  RendererClientBase::RunScriptsAtDocumentStart(render_frame);
  // Inform the document start phase.
  v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
  node::Environment* env = GetEnvironment(render_frame);
  if (env)
    gin_helper::EmitEvent(env->isolate(), env->process_object(),
                          "document-start");
}

void ElectronRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
  RendererClientBase::RunScriptsAtDocumentEnd(render_frame);
  // Inform the document end phase.
  v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
  node::Environment* env = GetEnvironment(render_frame);
  if (env)
    gin_helper::EmitEvent(env->isolate(), env->process_object(),
                          "document-end");
}

/*
It's possible
*/
void ElectronRendererClient::DidCreateScriptContext(
    v8::Handle<v8::Context> context,
    content::RenderFrame* render_frame) {
  // TODO(zcbenz): Do not create Node environment if node integration is not
  // enabled.

  // Only load Node.js if we are a main frame or a devtools extension
  // unless Node.js support has been explicitly enabled for subframes.
  if (!ShouldLoadPreload(context, render_frame))
    return;

  injected_frames_.insert(render_frame);

  if (!node_integration_initialized_) {
    node_integration_initialized_ = true;
    node_bindings_->Initialize(context);
    node_bindings_->PrepareEmbedThread();
  }

  // Setup node tracing controller.
  if (!node::tracing::TraceEventHelper::GetAgent())
    node::tracing::TraceEventHelper::SetAgent(node::CreateAgent());

  v8::Maybe<bool> initialized = node::InitializeContext(context);
  CHECK(!initialized.IsNothing() && initialized.FromJust());

  // If DidCreateScriptContext is called and we've already created a Node.js
  // Environment, then we're in the same process with a new V8::Context. We
  // should assign the existing Environment to the new V8::Context.
  if (env_) {
    env_->AssignToContext(context, nullptr, node::ContextInfo(""));
    return;
  }

  env_ = node_bindings_->CreateEnvironment(context, nullptr);

  // If we have disabled the site instance overrides we should prevent loading
  // any non-context aware native module.
  env_->options()->force_context_aware = true;

  // We do not want to crash the renderer process on unhandled rejections.
  env_->options()->unhandled_rejections = "warn-with-error-code";

  // Add Electron extended APIs.
  electron_bindings_->BindTo(env_->isolate(), env_->process_object());
  gin_helper::Dictionary process_dict(env_->isolate(), env_->process_object());
  BindProcess(env_->isolate(), &process_dict, render_frame);

  // Load the Environment.
  node_bindings_->LoadEnvironment(env_);

  // Make uv loop being wrapped by window context.
  node_bindings_->set_uv_env(env_);

  // Give the node loop a run to make sure everything is ready.
  node_bindings_->StartPolling();
}

void ElectronRendererClient::WillReleaseScriptContext(
    v8::Handle<v8::Context> context,
    content::RenderFrame* render_frame) {
  auto* env = node::Environment::GetCurrent(context);
  if (injected_frames_.erase(render_frame) == 0 || !env)
    return;

  gin_helper::EmitEvent(env_->isolate(), env_->process_object(), "exit");

  env->UntrackContext(context);
}

void ElectronRendererClient::WorkerScriptReadyForEvaluationOnWorkerThread(
    v8::Local<v8::Context> context) {
  // We do not create a Node.js environment in service or shared workers
  // owing to an inability to customize sandbox policies in these workers
  // given that they're run out-of-process.
  auto* ec = blink::ExecutionContext::From(context);
  if (ec->IsServiceWorkerGlobalScope() || ec->IsSharedWorkerGlobalScope())
    return;

  // This won't be correct for in-process child windows with webPreferences
  // that have a different value for nodeIntegrationInWorker
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNodeIntegrationInWorker)) {
    // WorkerScriptReadyForEvaluationOnWorkerThread can be invoked multiple
    // times for the same thread, so we need to create a new observer each time
    // this happens. We use a ThreadLocalOwnedPointer to ensure that the old
    // observer for a given thread gets destructed when swapping with the new
    // observer in WebWorkerObserver::Create.
    WebWorkerObserver::Create()->WorkerScriptReadyForEvaluation(context);
  }
}

void ElectronRendererClient::WillDestroyWorkerContextOnWorkerThread(
    v8::Local<v8::Context> context) {
  auto* ec = blink::ExecutionContext::From(context);
  if (ec->IsServiceWorkerGlobalScope() || ec->IsSharedWorkerGlobalScope())
    return;

  // TODO(loc): Note that this will not be correct for in-process child windows
  // with webPreferences that have a different value for nodeIntegrationInWorker
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNodeIntegrationInWorker)) {
    auto* current = WebWorkerObserver::GetCurrent();
    if (current)
      current->ContextWillDestroy(context);
  }
}

node::Environment* ElectronRendererClient::GetEnvironment(
    content::RenderFrame* render_frame) const {
  if (!base::Contains(injected_frames_, render_frame))
    return nullptr;
  return env_;
}

}  // namespace electron
