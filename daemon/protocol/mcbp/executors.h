/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#pragma once

#include <daemon/connection_mcbp.h>

/**
 * Implementation of the SELECT BUCKET command
 *
 * @param c the connection which should select the bucket
 * @param packet the incomming packet
 */
void select_bucket_executor(Cookie& cookie);

void list_bucket_executor(Cookie& cookie);

void get_cmd_timer_executor(Cookie& cookie);

void process_hello_packet_executor(Cookie& cookie);

// DCP executor
void dcp_add_stream_executor(Cookie& cookie);
void dcp_buffer_acknowledgement_executor(Cookie& cookie);
void dcp_close_stream_executor(Cookie& cookie);
void dcp_control_executor(Cookie& cookie);
void dcp_flush_executor(Cookie& cookie);
void dcp_get_failover_log_executor(Cookie& cookie);
void dcp_noop_executor(Cookie& cookie);
void dcp_open_executor(Cookie& cookie);
void dcp_set_vbucket_state_executor(Cookie& cookie);
void dcp_snapshot_marker_executor(Cookie& cookie);
void dcp_stream_end_executor(Cookie& cookie);
void dcp_stream_req_executor(Cookie& cookie);

// Collections
void collections_set_manifest_executor(Cookie& cookie);

void drop_privilege_executor(Cookie&);

void get_cluster_config_executor(Cookie&);
void set_cluster_config_executor(Cookie&);

void adjust_timeofday_executor(Cookie&);
