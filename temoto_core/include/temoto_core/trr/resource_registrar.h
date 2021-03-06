/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2020 TeMoto Telerobotics
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
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Author: Veiko Vunder */
/* Author: Robert Valner */

#ifndef TEMOTO_CORE__RESOURCE_REGISTRAR_H
#define TEMOTO_CORE__RESOURCE_REGISTRAR_H

#include "ros/callback_queue.h"
#include "temoto_core/common/base_subsystem.h"
#include "temoto_core/trr/resource_server.h"
#include "temoto_core/trr/resource_client.h"
#include "temoto_core/trr/resource_registrar_services.h"
#include <string>
#include <memory>  // dynamic_pointer_cast
#include <mutex>

namespace temoto_core
{
namespace trr
{

template <class Owner>
class ResourceRegistrar : public BaseSubsystem
{
  friend class BaseResourceServer<Owner>;

public:
  ResourceRegistrar(const std::string& name, Owner* owner)
  : BaseSubsystem(owner->subsystem_name_, owner->subsystem_code_, __func__, "trr." + owner->log_group_)
  , owner_(owner)
  , name_(name)
  , status_callback_(NULL)
  , status_spinner_(1, &status_cb_queue_)
  , unload_spinner_(1, &unload_cb_queue_)
  {
    /*
     * set up status callback with separately threaded queue
     */ 
    std::string status_srv_name = name_ + "/status";
    ros::AdvertiseServiceOptions status_service_opts =
        ros::AdvertiseServiceOptions::create<temoto_core::ResourceStatus>(
            status_srv_name, boost::bind(&ResourceRegistrar<Owner>::statusCallback, this, _1, _2),
            ros::VoidPtr(), &this->status_cb_queue_);
    status_server_ = nh_.advertiseService(status_service_opts);

    /*
     * set up unload callback with separately threaded queue
     */ 
    std::string unload_srv_name = name_ + "/unload";
    ros::AdvertiseServiceOptions unload_service_opts =
        ros::AdvertiseServiceOptions::create<temoto_core::UnloadResource>(
            unload_srv_name, boost::bind(&ResourceRegistrar<Owner>::unloadCallback, this, _1, _2),
            ros::VoidPtr(), &this->unload_cb_queue_);
    unload_server_ = nh_.advertiseService(unload_service_opts);

    /*
     * start separate threaded spinners for our callback queues
     */ 
    status_spinner_.start();
    unload_spinner_.start();
  }

// #ifdef enable_tracing
//   ResourceRegistrar(const std::string& name
//   , Owner* owner
//   , const std::string& tracer_lib_path
//   , const std::string& tracer_config_path)
//   {
//     ResourceRegistrar(name, owner);
//     try
//     {
//       initializeTracer(tracer_lib_path, tracer_config_path);
//     }
//     catch (error::ErrorStack& error_stack)
//     {
//       SEND_ERROR(error_stack);
//     }
//     catch(...)
//     {
//       TEMOTO_ERROR_STREAM("Unable to initialize the tracer");
//     }
//   }
// #endif

  ~ResourceRegistrar()
  {
    unloadClients();
    unload_spinner_.stop();
    status_spinner_.stop();
    #ifdef enable_tracing
    TRACER->Close();
    #endif
  }

  template <class ServiceType>
  bool addServer(const std::string& server_name,
                 void (Owner::*load_cb)(typename ServiceType::Request&,
                                        typename ServiceType::Response&),
                 void (Owner::*unload_cb)(typename ServiceType::Request&,
                                          typename ServiceType::Response&))
  {
    if (serverExists(server_name))
    {
      return false;
    }

    typedef std::shared_ptr<BaseResourceServer<Owner>> BaseResPtr;
    BaseResPtr res_srv = std::make_shared<ResourceServer<ServiceType, Owner>>(
        server_name, load_cb, unload_cb, owner_, *this);

    servers_.push_back(res_srv);

    return true;
  }

  void registerStatusCb(void (Owner::*status_cb)(temoto_core::ResourceStatus&))
  {
    status_callback_ = status_cb;
  }

  bool serverExists(const std::string server_name)
  {
    for (auto& server : servers_)
    {
      if (server->getName() == server_name)
      {
        return true;
      }
    }
    return false;
  }

  const std::string& getName()
  {
    return name_;
  }

  bool statusCallbackActive() const
  {
    return status_callback_active_;
  }

  #ifdef enable_tracing
  const temoto_core::StringMap& getStatusCallbackSpanContext() const
  {
    return status_callback_tracer_context_;
  }
  #endif

  template <class ServiceType>
  void call(std::string resource_registrar_name
  , std::string server_name
  , ServiceType& msg
  , trr::FailureBehavior failure_behavior = trr::FailureBehavior::NONE
  , std::string temoto_namespace = ::temoto_core::common::getTemotoNamespace()
  , temoto_core::StringMap parent_span_context = temoto_core::StringMap {})
  {
    using ClientType = ResourceClient<ServiceType, Owner>;
    using ClientPtr = std::shared_ptr<ClientType>;
    using BaseClientPtr = BaseResourceClientPtr<Owner>;

    ClientPtr client_ptr = NULL;
    waitForLock(clients_mutex_);

    #ifdef enable_tracing
    /*
     * Create a tracing span and see if this query is part of an active service routine
     */ 
    std::unique_ptr<opentracing::Span> tracing_span;

    if(!parent_span_context.empty())
    {
      TextMapCarrier carrier(parent_span_context);
      auto span_context_maybe = TRACER->Extract(carrier);
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});
    }
    else if (active_server_)
    {
      temoto_core::StringMap active_server_span_context = active_server_->getTracerSpanContext();
      TextMapCarrier carrier(active_server_span_context);
      auto span_context_maybe = TRACER->Extract(carrier);
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});
    }
    else if (statusCallbackActive())
    {
      TextMapCarrier carrier(status_callback_tracer_context_);
      auto span_context_maybe = TRACER->Extract(carrier);
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});
    }
    else
    {
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__);
    }
    #endif

    // check if this client already exists
    std::string client_name = '/'+temoto_namespace + "/" + resource_registrar_name + "/" + server_name;
    TEMOTO_TRACED_DEBUG("Calling '%s'.", client_name.c_str());

    typename std::vector<BaseClientPtr>::iterator client_it =
        std::find_if(clients_.begin(), clients_.end(),
                     [&](const BaseClientPtr& c) -> bool { return c->getName() == client_name; });
    if (client_it == clients_.end())
    {
      TEMOTO_TRACED_DEBUG("Creating new resource client '%s'.", client_name.c_str());

      client_ptr = std::make_shared<ClientType>(temoto_namespace, resource_registrar_name,
                                                server_name, owner_, *this);
      // Push to clients and convert to BaseResourceClient type
      clients_.push_back(client_ptr);
      client_it = std::prev(clients_.end());  // set iterator to the client pointer we just added
    }
    else
    {
      client_ptr = std::dynamic_pointer_cast<ClientType>(*client_it);
      if (!client_ptr)
      {
        // cast failed
        clients_mutex_.unlock();
        throw CREATE_ERROR(error::Code::RMP_FAIL, "Dynamic Cast failed, last time the same service was called using different type?");
      }
    }

    // generate new id for owner as the call was not initiated from any servers callback
    msg.response.trr.resource_id = generateID();
    
    try
    {
      #ifdef enable_tracing
      /*
       * Propagate the context of the span to the invoked subroutines
       * TODO: this segment of code will crash if the tracer is uninitialized
       */ 
      temoto_core::StringMap string_map;
      TextMapCarrier carrier(string_map);
      auto err = TRACER->Inject(tracing_span->context(), carrier);
      
      if(!err)
      {
        TEMOTO_WARN_STREAM("Failed to get the context of a tracing span");
      }
      else
      {
        // Convert it to a key value pair vector and send it to a child tracer
        msg.request.trr.tracer_context = temoto_core::unorderedMapToKeyValues(string_map);
      }
      #endif

      // make the call
      client_ptr->call(msg, failure_behavior);

      if (active_server_)
      {
        // if call was sucessful and the call was initiated from server callback,
        // link the client to the server
        TEMOTO_TRACED_DEBUG("Linking internal client.");
        active_server_->linkInternalResource(msg.response.trr.resource_id);
      }
    }
    catch (error::ErrorStack& error_stack)
    {
      clients_.erase(client_it);
      clients_mutex_.unlock();
      throw FORWARD_ERROR(error_stack);
    }

    clients_mutex_.unlock();
  }

  bool unloadCallback(temoto_core::UnloadResource::Request& req,
                      temoto_core::UnloadResource::Response& res)
  {
    TEMOTO_DEBUG("Unload request to server: '%s', ext id: %ld.", req.server_name.c_str(), req.resource_id);
    // Find server with requested name
    waitForLock(servers_mutex_);
    for (auto& server : servers_)
    {
      if (server->getName() == req.server_name)
      {
        server->unloadResource(req, res);
        TEMOTO_DEBUG("Resource %ld unloaded.", req.resource_id);
        break;
      }
    }
    servers_mutex_.unlock();
    return true;
  }

  void unloadClients()
  {
    waitForLock(clients_mutex_);
    TEMOTO_DEBUG("Number of clients:%lu", clients_.size());
    for (auto client : clients_)
    {
      //std::map<temoto_id::ID, FailureBehavior> internal_resources = client->getInternalResources();
      client->unloadResources();
      // TODO: remove all connections in servers
    }
    clients_.clear();
    clients_mutex_.unlock();
  }

  // Wrapper for unloading resource clients
  void unloadClientResource(temoto_id::ID resource_id)
  {
    waitForLock(clients_mutex_);

    #ifdef enable_tracing
    std::unique_ptr<opentracing::Span> tracing_span;
    if (status_callback_active_)
    {
      TextMapCarrier carrier(status_callback_tracer_context_);
      auto span_context_maybe = TRACER->Extract(carrier);
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});
    }
    else
    {
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__);
    }
    #endif

    TEMOTO_DEBUG("Unloading resource with id:'%d'.", resource_id);
    // Go through clients and search for given client by name
    auto client_it =
        std::find_if(clients_.begin(), clients_.end(),
                     [&](const std::shared_ptr<BaseResourceClient<Owner>>& client_ptr) -> bool {
                       return client_ptr->internalResourceExists(resource_id);
                     });
    if (client_it != clients_.end())
    {
      TEMOTO_DEBUG("Id:%d found, executing client -> unloadResource().", resource_id);
      // found the client, unload resource
      (*client_it)->unloadResource(resource_id);

   //   unlinkResource(resource_id);

      // when all resources for this client are removed, destroy this client
      if ((*client_it)->getQueryCount() <= 0)
      {
        TEMOTO_DEBUG("Destroying resource client '%s'", (*client_it)->getName().c_str());
        clients_.erase(client_it);
      }
    }
    else
    {
      TEMOTO_WARN("Internal id '%d' not found. Already removed?", resource_id);
    }
    clients_mutex_.unlock();
  }

  // This method sends error/info message to any client connected to this resource.
  void sendStatus(temoto_core::ResourceStatus& srv)
  {
    #ifdef enable_tracing
    /*
     * Create a tracing span and see if status message is intermediate or initial
     */ 
    std::unique_ptr<opentracing::Span> tracing_span;
    if (srv.request.tracer_context.empty())
    {
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__);
    }
    else
    {
      temoto_core::StringMap parent_status_span_context = keyValuesToUnorderedMap(srv.request.tracer_context);
      TextMapCarrier carrier(parent_status_span_context);
      auto span_context_maybe = TRACER->Extract(carrier);
      tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});
    }
    #endif

    TEMOTO_TRACED_DEBUG("Sending status to internal resource: %ld.", srv.request.resource_id);

    // For storing service and status topic temporarily.
    struct ResourceInfo
    {
      std::string status_topic;
      temoto_core::ResourceStatus srv;
    };
    std::vector<ResourceInfo> infos;

    waitForLock(servers_mutex_);
    for (const auto& server : servers_)
    {
      if (!server->hasInternalResource(srv.request.resource_id))
      {
        continue;
      }

      if (srv.request.status_code == trr::status_codes::FAILED)
      {
        // if this status message is about resource failure, mark the corresponding query as failed.
        server->setFailedFlag(srv.request.resource_id, srv.request.error_stack);
      }

      // Don't send status when it is adressed to server currently active and still in the middle of
      // processing the loadCallback. Instead, the above will store the error message in server
      // query response stack, which is returned when the load callback is processed.
      if (server == active_server_)
      {
        TEMOTO_TRACED_WARN("Skipping active server.");
        continue;
      }
      
      // get ext ID's and Topic pairs which correspond to the external resource ID
      auto ext_resources = server->getExternalResourcesByInternalId(srv.request.resource_id);
      ResourceInfo info;
      info.srv = srv;  // initialize with given values
      info.srv.request.temoto_namespace = ::temoto_core::common::getTemotoNamespace();
      info.srv.request.manager_name = name_;
      info.srv.request.server_name = server->getName();

      #ifdef enable_tracing
      /*
       * Propagate the context of the span to the invoked subroutines
       * TODO: this segment of code will crash if the tracer is uninitialized
       */ 
      temoto_core::StringMap string_map;
      TextMapCarrier carrier(string_map);
      auto err = TRACER->Inject(tracing_span->context(), carrier);
      
      if(!err)
      {
        TEMOTO_WARN_STREAM("Failed to get the context of a tracing span");
      }
      else
      {
        // Convert it to a key value pair vector and send it to a child tracer
        info.srv.request.tracer_context = temoto_core::unorderedMapToKeyValues(string_map);
      }
      #endif

      for (const auto& ext_resource : ext_resources)
      {
        TEMOTO_TRACED_WARN(" %d, %s ",ext_resource.first,ext_resource.second.c_str());
        // ext_resource.first ==> external resource id
        // ext_resource.second ==> status topic
        info.srv.request.resource_id = ext_resource.first;
        info.status_topic = ext_resource.second;
        infos.push_back(info);
      }
    }
    servers_mutex_.unlock();

    // forward status info to whoever is related with the given resource
    error::ErrorStack error_stack;
    for (auto& info : infos)
    {
      ros::ServiceClient service_client =
          nh_.serviceClient<temoto_core::ResourceStatus>(info.status_topic);
      TEMOTO_TRACED_DEBUG("Sending ResourceStatus to %s.", info.status_topic.c_str());
      if (service_client.call(info.srv))
      {
        TEMOTO_TRACED_DEBUG("ResourceStatus sucessfully sent to %s.", info.status_topic.c_str());
      }
      else
      {
        error_stack += CREATE_ERROR(error::Code::RMP_FAIL, "Failed to send ResourceStatus to %s.",
                                    info.status_topic.c_str());
      }
    }

    // commented since infos
   // if (infos.empty())
   // {
   //   error_stack += CREATE_ERROR(error::Code::RMP_FAIL, "Internal resource with id %ld was not found from any queries.",
   //                               srv.request.resource_id);
   // }

    if (!error_stack.empty())
    {
      throw error_stack;
    }
  }

// This method returns true when statusCallback() has set the FAILED flag to the given resource
  bool hasFailed(temoto_id::ID resource_id)
  {
    waitForLock(clients_mutex_);
    auto client_it =
        std::find_if(clients_.begin(), clients_.end(),
                     [&](const BaseResourceClientPtr<Owner>& client_ptr) -> bool {
                       return client_ptr->hasFailed(resource_id);
                     });
    bool has_failed =  client_it != clients_.end();
    clients_mutex_.unlock();
    return has_failed;
  }

  BaseResourceClientPtr<Owner> getClientByName(const std::string& client_name)
  {
    auto it = std::find_if(clients_.begin(), clients_.end(),
                           [&](const BaseResourceClientPtr<Owner>& client_ptr) -> bool {
                             return client_ptr->getName() == client_name;
                           });
    if (it == clients_.end())
    {
      throw CREATE_ERROR(error::Code::RMP_FAIL, "Client '%s' does not exist.", client_name);
    }
    return *it;
  }

  void unlinkResource(temoto_id::ID resource_id)
  {
    for (auto server:servers_)
    {
      if(server->isLinkedTo(resource_id))
      {
        server->unlinkInternalResource(resource_id);
      }
    }
  }

  bool statusCallback(temoto_core::ResourceStatus::Request& req,
                      temoto_core::ResourceStatus::Response& res)
  {
    status_callback_active_ = true;
    #ifdef enable_tracing
    /*
     * Create a span based on parent span and also extract the context of the created span
     */ 
    temoto_core::StringMap parent_status_span_context = keyValuesToUnorderedMap(req.tracer_context);
    TextMapCarrier carrier(parent_status_span_context);
    auto span_context_maybe = TRACER->Extract(carrier);
    std::unique_ptr<opentracing::Span> tracing_span = TRACER->StartSpan(this->class_name_ + "::" + __func__, {opentracing::ChildOf(span_context_maybe->get())});

    /*
     * Propagate the context of the span to the invoked subroutines
     * TODO: this segment of code will crash if the tracer is uninitialized
     */ 
    temoto_core::StringMap current_span_context_sm;
    TextMapCarrier current_context_carrier(current_span_context_sm);
    auto err = TRACER->Inject(tracing_span->context(), current_context_carrier);
    KeyValues current_span_context_kv;
    
    if(!err)
    {
      TEMOTO_WARN_STREAM("Failed to get the context of a tracing span");
    }
    else
    {
      // Convert it to a key value pair vector and send it to a child tracer
      current_span_context_kv = unorderedMapToKeyValues(current_span_context_sm);
      status_callback_tracer_context_ = current_span_context_sm;
    }
    #endif

    TEMOTO_TRACED_DEBUG("Got status request: "); 
    TEMOTO_DEBUG_STREAM(req);
    std::string client_name = "/" + req.temoto_namespace + "/" + req.manager_name + "/" + req.server_name;
    /*
     * 
     * if status == FAILED
     *  
     * * identify the query
     *   -mark it as Failed
     * * which internal clients used this query getInternalClients
     *    -send status to each of those. 
     * * if this was last internal resource, remove the query and call owners status callback?
     */

    // based on incoming external id, find the assigned internal client side ids
    // for each internal client side id find the external ids and do forwarding
    if (req.status_code == status_codes::FAILED)
    {
      // Go through clients and locate the one from
      // which the request arrived
      TEMOTO_TRACED_DEBUG("Got info that resource has failed, looking for client: '%s', external_resource_id: %ld", client_name.c_str(), req.resource_id);

      try
      {
        waitForLock(clients_mutex_);
        BaseResourceClientPtr<Owner> client_ptr = getClientByName(client_name);
        client_ptr->setFailedFlag(req.resource_id);
        // get internal ids which are related to the incoming external id from client side
        const auto int_resources = client_ptr->getInternalResources(req.resource_id);
        clients_mutex_.unlock();

        temoto_core::ResourceStatus srv;
        srv.request = req;
        srv.response = res;
        #ifdef enable_tracing
        srv.request.tracer_context = current_span_context_kv;
        #endif

        for (const auto& int_resource : int_resources)
        {
          // for each internal resource, execute owner's callback
          srv.request.resource_id = int_resource.first;
          if (status_callback_)
          {
            try
            {
              (owner_->*status_callback_)(srv);
            }
            catch (error::ErrorStack& error_stack)
            {
              TEMOTO_TRACED_ERROR("Caught an error from status callback.");
              res.error_stack += error_stack;
            }
          }

          try
          {
            // forward status info to whoever is linked to the given internal resource
            sendStatus(srv);

            // Take action based on resource failure behavior
            //switch (int_resource.second)
            //{
            //  case trr::FailureBehavior::UNLOAD:
            //    TEMOTO_TRACED_WARN("UNLOADING LINKED RESOURCE %d", int_resource.first);
            //    unlinkResource(int_resource.first);

            //    break;
            //  case trr::FailureBehavior::RELOAD:
            //    TEMOTO_TRACED_WARN("UNLOADING LINKED RESOURCE AND RELOADING %d", int_resource.first);
            //    unlinkResource(int_resource.first);
            //    // now try to reload.
            //    break;
            //  default:
            //    TEMOTO_TRACED_WARN("DEFAULT BEHAVIOR RESOURCE %d", int_resource.first);
            //}
          }
          catch (error::ErrorStack& error_stack)
          {
            res.error_stack += FORWARD_ERROR(error_stack);
          }
        }
      }
      catch (error::ErrorStack& error_stack)
      {
        clients_mutex_.unlock();
        TEMOTO_TRACED_ERROR("An extreme badness was captured...");
        res.error_stack += error_stack;
        return true;
      }

    }  // if code == FAILED

    else if (req.status_code == status_codes::UPDATE)
    {
      waitForLock(clients_mutex_);
      BaseResourceClientPtr<Owner> client_ptr = getClientByName(client_name);
      // get internal ids which are related to the incoming external id from client side
      const auto int_resources = client_ptr->getInternalResources(req.resource_id);
      clients_mutex_.unlock();

      temoto_core::ResourceStatus srv;
      srv.request = req;
      srv.response = res;
      #ifdef enable_tracing
      srv.request.tracer_context = current_span_context_kv;
      #endif

      for (const auto& int_resource : int_resources)
      {
        // for each internal resource, execute owner's callback
        srv.request.resource_id = int_resource.first;
        if (status_callback_)
        {
          try
          {
            (owner_->*status_callback_)(srv);
          }
          catch (error::ErrorStack& error_stack)
          {
            TEMOTO_TRACED_ERROR("Caught an error from status callback.");
            res.error_stack += error_stack;
          }
        }

        try
        {
          // forward status info to whoever is linked to the given internal resource
          sendStatus(srv);

          // Take action based on resource failure behavior
          //switch (int_resource.second)
          //{
          //  case trr::FailureBehavior::UNLOAD:
          //    TEMOTO_TRACED_WARN("UNLOADING LINKED RESOURCE %d", int_resource.first);
          //    unlinkResource(int_resource.first);

          //    break;
          //  case trr::FailureBehavior::RELOAD:
          //    TEMOTO_TRACED_WARN("UNLOADING LINKED RESOURCE AND RELOADING %d", int_resource.first);
          //    unlinkResource(int_resource.first);
          //    // now try to reload.
          //    break;
          //  default:
          //    TEMOTO_TRACED_WARN("DEFAULT BEHAVIOR RESOURCE %d", int_resource.first);
          //}
        }
        catch (error::ErrorStack& error_stack)
        {
          res.error_stack += FORWARD_ERROR(error_stack);
        }
      }
    }

    status_callback_active_ = false;
    return true;
  }

//  std::vector<std::pair<temoto_id::ID, std::string>>
//  getServerExtResources(temoto_id::ID internal_resource_id, const std::string& server_name)
//  {
//    waitForLock(servers_mutex_);
//    std::vector<std::pair<temoto_id::ID, std::string>> ext_resources;
//    auto s_it = find_if(servers_.begin(), servers_.end(),
//                        [&](const std::shared_ptr<BaseResourceServer<Owner>>& server_ptr) -> bool {
//                          return server_ptr->getName() == server_name;
//                        });
//    if (s_it != servers_.end())
//    {
//      try
//      {
//        ext_resources = (*s_it)->getExternalResources(internal_resource_id);
//      }
//      catch(error::ErrorStack& error_stack)
//      {
//        servers_mutex_.unlock();
//        throw FORWARD_ERROR(error_stack);
//      }
//    }
//    servers_mutex_.unlock();
//    return ext_resources;
//  }
//
  temoto_id::ID generateID()
  {
    std::lock_guard<std::mutex> lock(id_manager_mutex_);
    return id_manager_.generateID();
  }

  const std::string getActiveServerName() const
  {
    if (active_server_)
    {
      return active_server_->getName();
    }
    return std::string("");
  }

private:
  void setActiveServer(BaseResourceServer<Owner>* active_server)
  {
    // inactivate server;
    if (!active_server)
    {
      active_server_ = NULL;
      return;
    }

    std::lock_guard<std::mutex> lock(active_server_mutex_);
    for (auto& server_shared_ptr : servers_)
    {
      if (server_shared_ptr.get() == active_server)
      {
        active_server_ = server_shared_ptr;
        return;
      }
    }
    CREATE_ERROR(error::Code::RMP_FAIL, "Resource server '%s' not found.", active_server->getName());
  }

  void waitForLock(std::mutex& m)
  {
    while (!m.try_lock())
    {
      TEMOTO_DEBUG("Waiting for lock()"); 
      ros::Duration(0.2).sleep();  // sleep for few ms
    }
  }

  std::vector<std::shared_ptr<BaseResourceServer<Owner>>> servers_;
  std::vector<std::shared_ptr<BaseResourceClient<Owner>>> clients_;
  std::string name_;
  Owner* owner_;
  temoto_id::IDManager id_manager_;
  std::shared_ptr<BaseResourceServer<Owner>> active_server_;
  void (Owner::*status_callback_)(temoto_core::ResourceStatus&);
  bool status_callback_active_ = false;

  #ifdef enable_tracing
  temoto_core::StringMap status_callback_tracer_context_;
  #endif

  ros::AsyncSpinner status_spinner_;
  ros::AsyncSpinner unload_spinner_;
  ros::CallbackQueue status_cb_queue_;
  ros::CallbackQueue unload_cb_queue_;
  ros::NodeHandle nh_;
  ros::ServiceServer unload_server_;
  ros::ServiceServer status_server_;

  // thread safety locks;
  std::mutex id_manager_mutex_;
  std::mutex servers_mutex_;
  std::mutex clients_mutex_;
  std::mutex active_server_mutex_;
};

} // namespace trr
} // namespace temoto_core

#endif
