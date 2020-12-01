/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/resolver_registry.h"

#include <string.h>

#include "absl/container/inlined_vector.h"
#include "absl/strings/str_cat.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

namespace grpc_core {

namespace {

class RegistryState {
 public:
  RegistryState() : default_prefix_(gpr_strdup("dns:///")) {}

  void SetDefaultPrefix(const char* default_resolver_prefix) {
    GPR_ASSERT(default_resolver_prefix != nullptr);
    GPR_ASSERT(*default_resolver_prefix != '\0');
    default_prefix_.reset(gpr_strdup(default_resolver_prefix));
  }

  void RegisterResolverFactory(std::unique_ptr<ResolverFactory> factory) {
    for (size_t i = 0; i < factories_.size(); ++i) {
      GPR_ASSERT(strcmp(factories_[i]->scheme(), factory->scheme()) != 0);
    }
    factories_.push_back(std::move(factory));
  }

  ResolverFactory* LookupResolverFactory(const char* scheme) const {
    for (size_t i = 0; i < factories_.size(); ++i) {
      if (strcmp(scheme, factories_[i]->scheme()) == 0) {
        return factories_[i].get();
      }
    }
    return nullptr;
  }

  // Returns the factory for the scheme of \a target.  If \a target does
  // not parse as a URI, prepends \a default_prefix_ and tries again.
  // If URI parsing is successful (in either attempt), sets \a uri to
  // point to the parsed URI.
  // If \a default_prefix_ needs to be prepended, sets \a canonical_target
  // to the canonical target string.
  ResolverFactory* FindResolverFactory(const char* target, grpc_uri** uri,
                                       std::string* canonical_target) const {
    GPR_ASSERT(uri != nullptr);
    *uri = grpc_uri_parse(target, true);
    ResolverFactory* factory =
        *uri == nullptr ? nullptr : LookupResolverFactory((*uri)->scheme);
    if (factory == nullptr) {
      grpc_uri_destroy(*uri);
      *canonical_target = absl::StrCat(default_prefix_.get(), target);
      *uri = grpc_uri_parse(canonical_target->c_str(), true);
      factory =
          *uri == nullptr ? nullptr : LookupResolverFactory((*uri)->scheme);
      if (factory == nullptr) {
        grpc_uri_destroy(grpc_uri_parse(target, false));
        grpc_uri_destroy(grpc_uri_parse(canonical_target->c_str(), false));
        gpr_log(GPR_ERROR, "don't know how to resolve '%s' or '%s'", target,
                canonical_target->c_str());
      }
    }
    return factory;
  }

 private:
  // We currently support 10 factories without doing additional
  // allocation.  This number could be raised if there is a case where
  // more factories are needed and the additional allocations are
  // hurting performance (which is unlikely, since these allocations
  // only occur at gRPC initialization time).
  absl::InlinedVector<std::unique_ptr<ResolverFactory>, 10> factories_;
  grpc_core::UniquePtr<char> default_prefix_;
};

static RegistryState* g_state = nullptr;

}  // namespace

//
// ResolverRegistry::Builder
//

void ResolverRegistry::Builder::InitRegistry() {
  if (g_state == nullptr) g_state = new RegistryState();
}

void ResolverRegistry::Builder::ShutdownRegistry() {
  delete g_state;
  g_state = nullptr;
}

void ResolverRegistry::Builder::SetDefaultPrefix(const char* default_prefix) {
  InitRegistry();
  g_state->SetDefaultPrefix(default_prefix);
}

void ResolverRegistry::Builder::RegisterResolverFactory(
    std::unique_ptr<ResolverFactory> factory) {
  InitRegistry();
  g_state->RegisterResolverFactory(std::move(factory));
}

//
// ResolverRegistry
//

ResolverFactory* ResolverRegistry::LookupResolverFactory(const char* scheme) {
  GPR_ASSERT(g_state != nullptr);
  return g_state->LookupResolverFactory(scheme);
}

bool ResolverRegistry::IsValidTarget(const char* target) {
  grpc_uri* uri = nullptr;
  std::string canonical_target;
  ResolverFactory* factory =
      g_state->FindResolverFactory(target, &uri, &canonical_target);
  bool result = factory == nullptr ? false : factory->IsValidUri(uri);
  grpc_uri_destroy(uri);
  return result;
}

OrphanablePtr<Resolver> ResolverRegistry::CreateResolver(
    const char* target, const grpc_channel_args* args,
    grpc_pollset_set* pollset_set,
    std::shared_ptr<WorkSerializer> work_serializer,
    std::unique_ptr<Resolver::ResultHandler> result_handler) {
  GPR_ASSERT(g_state != nullptr);
  grpc_uri* uri = nullptr;
  std::string canonical_target;
  ResolverFactory* factory =
      g_state->FindResolverFactory(target, &uri, &canonical_target);
  ResolverArgs resolver_args;
  resolver_args.uri = uri;
  resolver_args.args = args;
  resolver_args.pollset_set = pollset_set;
  resolver_args.work_serializer = std::move(work_serializer);
  resolver_args.result_handler = std::move(result_handler);
  OrphanablePtr<Resolver> resolver =
      factory == nullptr ? nullptr
                         : factory->CreateResolver(std::move(resolver_args));
  grpc_uri_destroy(uri);
  return resolver;
}

grpc_core::UniquePtr<char> ResolverRegistry::GetDefaultAuthority(
    const char* target) {
  GPR_ASSERT(g_state != nullptr);
  grpc_uri* uri = nullptr;
  std::string canonical_target;
  ResolverFactory* factory =
      g_state->FindResolverFactory(target, &uri, &canonical_target);
  grpc_core::UniquePtr<char> authority =
      factory == nullptr ? nullptr : factory->GetDefaultAuthority(uri);
  grpc_uri_destroy(uri);
  return authority;
}

grpc_core::UniquePtr<char> ResolverRegistry::AddDefaultPrefixIfNeeded(
    const char* target) {
  GPR_ASSERT(g_state != nullptr);
  grpc_uri* uri = nullptr;
  std::string canonical_target;
  g_state->FindResolverFactory(target, &uri, &canonical_target);
  grpc_uri_destroy(uri);
  return grpc_core::UniquePtr<char>(canonical_target.empty()
                                        ? gpr_strdup(target)
                                        : gpr_strdup(canonical_target.c_str()));
}

}  // namespace grpc_core
