/*
  Copyright (c) 2022 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Authors: Wang Zhenpeng (wangzhenpeng@sogou-inc.com)
*/

#include "WFConsulManager.h"

using namespace protocol;

int WFConsulManager::init(const std::string& proxy_url)
{
	return this->client.init(proxy_url);
}

int WFConsulManager::init(const std::string& proxy_url, ConsulConfig config)
{
	return this->client.init(proxy_url, std::move(config));
}

void WFConsulManager::deinit()
{
	this->client.deinit();
}

int WFConsulManager::watch_service(const std::string& service_namespace,
								   const std::string& service_name)
{
	struct AddressParams address_params = ADDRESS_PARAMS_DEFAULT;
	return this->watch_service(service_namespace, service_name, &address_params);
}

int WFConsulManager::watch_service(const std::string& service_namespace,
								   const std::string& service_name,
								   const struct AddressParams *address_params)
{	
	std::string policy_name = get_policy_name(service_namespace, service_name);

	if (is_watched(policy_name))
		return 0;

	if (UpstreamManager::upstream_create_vnswrr(policy_name) != 0)
		return -1;

	WFFacilities::WaitGroup wait_group(1);
	auto&& discover_cb = std::bind(&WFConsulManager::discover_callback, this,
								   std::placeholders::_1);
	WFConsulTask *task;
	task = this->client.create_discover_task(service_namespace, service_name,
											 this->retry_max,
											 std::move(discover_cb));
	task->set_consul_index(0);

	struct ConsulCallBackResult result;
	result.wait_group = &wait_group;
	task->user_data = &result;

	struct WatchContext *ctx = new struct WatchContext;
	ctx->service_namespace = service_namespace;
	ctx->service_name = service_name;
	ctx->address_params = *address_params;

	SeriesWork *series = Workflow::create_series_work(task,
										[](const SeriesWork *series) {
		delete (struct WatchContext *)series->get_context();
	});

	series->set_context(ctx);
	series->start();

	wait_group.wait();
	this->mutex.lock();
	this->watch_status[policy_name]->watch_state = CONSUL_WATCHING;
	this->mutex.unlock();

	return result.error == WFT_STATE_SUCCESS ? 0 : -1;
}

int WFConsulManager::unwatch_service(const std::string& service_namespace,
									 const std::string& service_name)
{
	std::string policy_name = get_policy_name(service_namespace, service_name);
	std::unique_lock<std::mutex> lock(this->mutex);
	auto iter = this->watch_status.find(policy_name);
	if (iter != this->watch_status.end())
	{
		if (iter->second->watch_state == CONSUL_WATCHING)
		{
			iter->second->watch_state = CONSUL_UNWATCH;
			iter->second->cond.wait(lock);
		}

		delete iter->second;
		this->watch_status.erase(iter);
	}
	lock.unlock();

	return UpstreamManager::upstream_delete(policy_name);
}

int WFConsulManager::register_service(const struct ConsulService *service)
{
	WFFacilities::WaitGroup wait_group(1);

	auto&& register_cb = std::bind(&WFConsulManager::register_callback, this,
								   std::placeholders::_1);
	WFConsulTask *task;
	task = this->client.create_register_task(service->service_namespace,
											 service->service_name,
											 service->service_id,
											 this->retry_max,
											 std::move(register_cb));
	task->set_service(service);

	struct ConsulCallBackResult result;
	result.wait_group = &wait_group;
	task->user_data = &result;
	task->start();

	wait_group.wait();

	return result.error == WFT_STATE_SUCCESS ? 0 : -1;
}

int WFConsulManager::deregister_service(const std::string& service_namespace,
										const std::string& service_id)
{
	WFFacilities::WaitGroup wait_group(1);

	auto&& deregister_cb = std::bind(&WFConsulManager::register_callback, this,
									 std::placeholders::_1);
	WFConsulTask *task = this->client.create_deregister_task(service_namespace,
															 service_id,
															 this->retry_max,
															 deregister_cb);

	struct ConsulCallBackResult result;
	result.wait_group = &wait_group;
	task->user_data = &result;
	task->start();

	wait_group.wait();

	return result.error == WFT_STATE_SUCCESS ? 0 : -1;
}

void WFConsulManager::get_watching_services(std::vector<std::string>& services)
{
	this->mutex.lock();
	for (const auto& kv : this->watch_status)
	{
		services.emplace_back(kv.first);
	}
	this->mutex.unlock();
}

WFTimerTask *WFConsulManager::create_timer_task(long long consul_index)
{
	auto timer_cb = std::bind(&WFConsulManager::timer_callback, this,
							  std::placeholders::_1, consul_index);
	int ttl = 0;
	if (!this->config.blocking_query())
		ttl = this->config.get_wait_ttl() * 1000;
	WFTimerTask *timer_task = WFTaskFactory::create_timer_task(ttl, timer_cb);
	return timer_task;
}

void WFConsulManager::process_watch_info(WFConsulTask *task,
										ConsulInstances& instances)
{
	struct WatchContext *ctx =
		(struct WatchContext *)series_of(task)->get_context();
	std::string policy_name = get_policy_name(ctx->service_namespace,
											  ctx->service_name);
	long long consul_index = task->get_consul_index();

	struct WatchInfo *watch_info = NULL;
	bool need_update = true;
	bool unwatch = false;
	this->mutex.lock();
	auto iter = this->watch_status.find(policy_name);
	if (iter != this->watch_status.end())
	{
		if (iter->second->consul_index >= consul_index)
			need_update = false;

		watch_info = iter->second;
	}
	else
	{
		watch_info = new struct WatchInfo;
		this->watch_status[policy_name] = watch_info;
	}

	watch_info->consul_index = consul_index;

	if (need_update)
	{
		update_upstream_and_instances(policy_name, instances,
				&ctx->address_params, watch_info->cached_addresses);
	}

	if (watch_info->watch_state == CONSUL_UNWATCH)
	{
		unwatch = true;
		watch_info->cond.notify_one();
	}
	this->mutex.unlock();

	if (!unwatch)
		series_of(task)->push_back(create_timer_task(consul_index));
}

void WFConsulManager::discover_callback(WFConsulTask *task)
{
	int state = task->get_state();
	int error = task->get_error();

	bool ret = false;
	ConsulInstances instances;
	if (state == WFT_STATE_SUCCESS)
		ret = task->get_discover_result(instances);

	if (ret)
	{
		process_watch_info(task, instances);
	}

	if (task->user_data)
	{
		struct ConsulCallBackResult *result = NULL;
		result = (struct ConsulCallBackResult*)task->user_data;
		result->error = error;
		if (result->error == 0 && !ret)
			result->error = -1;

		result->wait_group->done();
	}
}

void WFConsulManager::timer_callback(WFTimerTask *task, long long consul_index)
{
	struct WatchContext *ctx =
		(struct WatchContext *)series_of(task)->get_context();

	auto&& discover_cb = std::bind(&WFConsulManager::discover_callback, this,
								 std::placeholders::_1);
	WFConsulTask *discover_task;
	discover_task = this->client.create_discover_task(ctx->service_namespace,
													  ctx->service_name,
													  this->retry_max,
													  discover_cb);
	discover_task->set_consul_index(consul_index);
	series_of(task)->push_back(discover_task);
}

void WFConsulManager::register_callback(WFConsulTask *task)
{
	int error = task->get_error();

	struct ConsulCallBackResult *result;
	result = (struct ConsulCallBackResult *)task->user_data;
	result->error = error;
	result->wait_group->done();
	return;
}

int WFConsulManager::update_upstream_and_instances(
							const std::string& policy_name,
							const ConsulInstances& instances,
							const struct AddressParams *address_params,
							std::unordered_set<std::string>& cached_addresses)
{
	std::vector<std::string> add_addresses, remove_addresses;
	std::unordered_set<std::string> cur_address_set;
	std::string	address;
	for (const auto& instance : instances)
	{
		address = get_address(instance.service.service_address.first,
						   instance.service.service_address.second);
		if (cached_addresses.find(address) == cached_addresses.end())
			add_addresses.emplace_back(address);

		cur_address_set.insert(address);
	}

	for (const auto& address : cached_addresses)
	{
		if (cur_address_set.find(address) == cur_address_set.end())
			remove_addresses.emplace_back(address);
	}

	cached_addresses.swap(cur_address_set);

	int ret = 0;
	ret += add_servers(policy_name, add_addresses, address_params);
	ret += remove_servers(policy_name, remove_addresses);
	return ret;
}

std::string WFConsulManager::get_policy_name(const std::string& service_namespace,
											 const std::string& service_name)
{
	return service_namespace + "." + service_name;
}

std::string WFConsulManager::get_address(const std::string& host,
										 unsigned short port)
{
	return host + ":" + std::to_string(port);
}

int WFConsulManager::add_servers(const std::string& policy_name,
								 const std::vector<std::string>& addresses,
								 const struct AddressParams *address_params)
{
	for (const auto& address : addresses)
	{
		fprintf(stderr, "add addr:%s\n", address.c_str());
		if (UpstreamManager::upstream_add_server(policy_name, address,
												 address_params) == -1)
			return -1;
	}

	return 0;
}

int WFConsulManager::remove_servers(const std::string& policy_name,
									const std::vector<std::string>& addresses)
{
	for (const auto& address : addresses)
	{
		fprintf(stderr, "remove addr:%s\n", address.c_str());
		if (UpstreamManager::upstream_remove_server(policy_name,
													address) == -1)
			return -1;
	}

	return 0;
}

bool WFConsulManager::is_watched(const std::string& policy_name)
{
	bool watched = false;
	this->mutex.lock();
	auto iter = this->watch_status.find(policy_name);
	if (iter != this->watch_status.end())
		watched = true;	
	this->mutex.unlock();
	return watched;
}
