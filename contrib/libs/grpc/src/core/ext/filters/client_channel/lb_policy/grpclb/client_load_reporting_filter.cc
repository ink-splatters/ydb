/*
 *
 * Copyright 2017 gRPC authors.
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

#include "src/core/ext/filters/client_channel/lb_policy/grpclb/client_load_reporting_filter.h"

#include <new>

#include "y_absl/types/optional.h"

#include <grpc/support/log.h>

#include "src/core/ext/filters/client_channel/lb_policy/grpclb/grpclb_client_stats.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/transport.h"

static grpc_error_handle clr_init_channel_elem(
    grpc_channel_element* /*elem*/, grpc_channel_element_args* /*args*/) {
  return GRPC_ERROR_NONE;
}

static void clr_destroy_channel_elem(grpc_channel_element* /*elem*/) {}

namespace {

struct call_data {
  // Stats object to update.
  grpc_core::RefCountedPtr<grpc_core::GrpcLbClientStats> client_stats;
  // State for intercepting send_initial_metadata.
  grpc_closure on_complete_for_send;
  grpc_closure* original_on_complete_for_send;
  bool send_initial_metadata_succeeded = false;
  // State for intercepting recv_initial_metadata.
  grpc_closure recv_initial_metadata_ready;
  grpc_closure* original_recv_initial_metadata_ready;
  bool recv_initial_metadata_succeeded = false;
};

}  // namespace

static void on_complete_for_send(void* arg, grpc_error_handle error) {
  call_data* calld = static_cast<call_data*>(arg);
  if (GRPC_ERROR_IS_NONE(error)) {
    calld->send_initial_metadata_succeeded = true;
  }
  grpc_core::Closure::Run(DEBUG_LOCATION, calld->original_on_complete_for_send,
                          GRPC_ERROR_REF(error));
}

static void recv_initial_metadata_ready(void* arg, grpc_error_handle error) {
  call_data* calld = static_cast<call_data*>(arg);
  if (GRPC_ERROR_IS_NONE(error)) {
    calld->recv_initial_metadata_succeeded = true;
  }
  grpc_core::Closure::Run(DEBUG_LOCATION,
                          calld->original_recv_initial_metadata_ready,
                          GRPC_ERROR_REF(error));
}

static grpc_error_handle clr_init_call_elem(
    grpc_call_element* elem, const grpc_call_element_args* args) {
  GPR_ASSERT(args->context != nullptr);
  new (elem->call_data) call_data();
  return GRPC_ERROR_NONE;
}

static void clr_destroy_call_elem(grpc_call_element* elem,
                                  const grpc_call_final_info* /*final_info*/,
                                  grpc_closure* /*ignored*/) {
  call_data* calld = static_cast<call_data*>(elem->call_data);
  if (calld->client_stats != nullptr) {
    // Record call finished, optionally setting client_failed_to_send and
    // received.
    calld->client_stats->AddCallFinished(
        !calld->send_initial_metadata_succeeded /* client_failed_to_send */,
        calld->recv_initial_metadata_succeeded /* known_received */);
  }
  calld->~call_data();
}

static void clr_start_transport_stream_op_batch(
    grpc_call_element* elem, grpc_transport_stream_op_batch* batch) {
  call_data* calld = static_cast<call_data*>(elem->call_data);
  // Handle send_initial_metadata.
  if (batch->send_initial_metadata) {
    // Grab client stats object from metadata.
    auto client_stats_md =
        batch->payload->send_initial_metadata.send_initial_metadata->Take(
            grpc_core::GrpcLbClientStatsMetadata());
    if (client_stats_md.has_value()) {
      grpc_core::GrpcLbClientStats* client_stats = *client_stats_md;
      if (client_stats != nullptr) {
        calld->client_stats.reset(client_stats);
        // Intercept completion.
        calld->original_on_complete_for_send = batch->on_complete;
        GRPC_CLOSURE_INIT(&calld->on_complete_for_send, on_complete_for_send,
                          calld, grpc_schedule_on_exec_ctx);
        batch->on_complete = &calld->on_complete_for_send;
      }
    }
  }
  // Intercept completion of recv_initial_metadata.
  if (batch->recv_initial_metadata) {
    calld->original_recv_initial_metadata_ready =
        batch->payload->recv_initial_metadata.recv_initial_metadata_ready;
    GRPC_CLOSURE_INIT(&calld->recv_initial_metadata_ready,
                      recv_initial_metadata_ready, calld,
                      grpc_schedule_on_exec_ctx);
    batch->payload->recv_initial_metadata.recv_initial_metadata_ready =
        &calld->recv_initial_metadata_ready;
  }
  // Chain to next filter.
  grpc_call_next_op(elem, batch);
}

const grpc_channel_filter grpc_client_load_reporting_filter = {
    clr_start_transport_stream_op_batch,
    nullptr,
    grpc_channel_next_op,
    sizeof(call_data),
    clr_init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    clr_destroy_call_elem,
    0,  // sizeof(channel_data)
    clr_init_channel_elem,
    grpc_channel_stack_no_post_init,
    clr_destroy_channel_elem,
    grpc_channel_next_get_info,
    "client_load_reporting"};
