//
//
// Tencent is pleased to support the open source community by making tRPC available.
//
// Copyright (C) 2023 THL A29 Limited, a Tencent company.
// All rights reserved.
//
// If you have downloaded a copy of the tRPC source code from Tencent,
// please note that tRPC source code is licensed under the  Apache 2.0 License,
// A copy of the Apache 2.0 License is included in this file.
//
//

#include "trpc/naming/common/util/loadbalance/hash/modulohash_load_balance.h"
#include "trpc/naming/common/util/loadbalance/hash/hash_func.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <any>
#include <iostream>
#include <regex>
#include <vector>

#include "trpc/naming/load_balance_factory.h"
#include "trpc/util/log/logging.h"

namespace trpc {

bool ModuloHashLoadBalance::IsLoadBalanceInfoDiff(const LoadBalanceInfo* info) {
  if (nullptr == info || nullptr == info->info || nullptr == info->endpoints) {
    return false;
  }

  const SelectorInfo* select_info = info->info;
  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (callee_router_infos_.end() == callee_router_infos_.find(select_info->name)) {
    return true;
  }

  std::vector<TrpcEndpointInfo>& orig_endpoints = callee_router_infos_[select_info->name].endpoints;

  const std::vector<TrpcEndpointInfo>* new_endpoints = info->endpoints;
  if (orig_endpoints.size() != new_endpoints->size()) {
    return true;
  }

  int i = 0;
  for (auto& var : *new_endpoints) {
    auto orig_endpoint = orig_endpoints[i++];
    if (orig_endpoint.host != var.host || orig_endpoint.port != var.port) {
      return true;
    }

    if (orig_endpoint.status != var.status) {
      return true;
    }
  }

  return false;
}

std::string ModuloHashLoadBalance::GenerateKeysAsString(const SelectorInfo* info, std::vector<uint32_t> indexs) {
  std::string key;
  for (int index : indexs) {
    switch (index) {
      case 0:
        key += info->name;
      case 1:
        key += std::to_string(static_cast<int>(info->policy));
      case 2:
        key += std::to_string(info->select_num);
      case 3:
        key += info->load_balance_name;
      case 4:
        key += std::to_string(info->is_from_workflow);
    }
  }
  return key;
}

std::uint64_t ModuloHashLoadBalance::Hash(const std::string& input, uint64_t num, const std::string& hash_func) {
  if (hash_func == MD5HASH) {
    return MD5Hash(input) % num;
  } else if (hash_func == BKDRHASH) {
    return BKDRHash(input) % num;
  } else if (hash_func == FNV1AHASH) {
    return FNV1aHash(input) % num;
  } else {
    return MurMurHash3(input) % num;
  }
}

// Update the routing nodes used for load balancing
int ModuloHashLoadBalance::Update(const LoadBalanceInfo* info) {
  if (nullptr == info || nullptr == info->info || nullptr == info->endpoints) {
    TRPC_LOG_ERROR("Endpoint info of name is empty");
    return -1;
  }

  naming::LoadBalanceSelectorConfig loadbalance_config_;

  const SelectorInfo* select_info = info->info;

  const std::any* extend_select_info = select_info->extend_select_info;

  // if load balance config is exist,and init it
  if (extend_select_info != nullptr && extend_select_info->type() == typeid(naming::LoadBalanceSelectorConfig)) {
    loadbalance_config_ = std::any_cast<naming::LoadBalanceSelectorConfig>(*extend_select_info);
  }

  if (IsLoadBalanceInfoDiff(info)) {
    InnerEndpointInfos endpoint_info;

    endpoint_info.endpoints.assign(info->endpoints->begin(), info->endpoints->end());

    endpoint_info.hash = Hash(GenerateKeysAsString(select_info, loadbalance_config_.hash_args),
                              endpoint_info.endpoints.size(), loadbalance_config_.hash_func);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    callee_router_infos_[select_info->name] = endpoint_info;
  }

  return 0;
}

int ModuloHashLoadBalance::Next(LoadBalanceResult& result) {
  if (nullptr == result.info) {
    return -1;
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto iter = callee_router_infos_.find((result.info)->name);
  if (iter == callee_router_infos_.end()) {
    TRPC_LOG_ERROR("Router info of name " << (result.info)->name << " no found");
    return -1;
  }

  std::vector<TrpcEndpointInfo>& endpoints = iter->second.endpoints;
  size_t endpoints_num = endpoints.size();
  if (endpoints_num < 1) {
    TRPC_LOG_ERROR("Router info of name is empty");
    return -1;
  }

  uint32_t index = 0;
  uint32_t id = __sync_fetch_and_add(&iter->second.hash, 1);
  index = id % endpoints_num;

  result.result = endpoints[index];

  return 0;
}

}  // namespace trpc