/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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
#include "mcbp.h"

#include "debug_helpers.h"
#include "memcached.h"
#include "protocol/mcbp/engine_wrapper.h"
#include "utilities/protocol2text.h"
#include "xattr/utils.h"

#include <mcbp/protocol/framebuilder.h>
#include <mcbp/protocol/header.h>
#include <platform/compress.h>

static cb::const_byte_buffer mcbp_add_header(cb::Pipe& pipe,
                                             uint8_t opcode,
                                             uint16_t err,
                                             uint8_t ext_len,
                                             uint16_t key_len,
                                             uint32_t body_len,
                                             uint8_t datatype,
                                             uint32_t opaque,
                                             uint64_t cas) {
    auto wbuf = pipe.wdata();
    auto* header = (protocol_binary_response_header*)wbuf.data();

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = opcode;
    header->response.keylen = (uint16_t)htons(key_len);

    header->response.extlen = ext_len;
    header->response.datatype = datatype;
    header->response.status = (uint16_t)htons(err);

    header->response.bodylen = htonl(body_len);
    header->response.opaque = opaque;
    header->response.cas = htonll(cas);
    pipe.produced(sizeof(header->bytes));

    return {wbuf.data(), sizeof(header->bytes)};
}

void mcbp_add_header(Cookie& cookie,
                     uint16_t err,
                     uint8_t ext_len,
                     uint16_t key_len,
                     uint32_t body_len,
                     uint8_t datatype) {
    auto& connection = cookie.getConnection();
    connection.addMsgHdr(true);
    const auto& header = cookie.getHeader();
    const auto wbuf = mcbp_add_header(*connection.write,
                                      header.getOpcode(),
                                      err,
                                      ext_len,
                                      key_len,
                                      body_len,
                                      datatype,
                                      header.getOpaque(),
                                      cookie.getCas());

    if (settings.getVerbose() > 1) {
        char buffer[1024];
        if (bytes_to_output_string(buffer,
                                   sizeof(buffer),
                                   connection.getId(),
                                   false,
                                   "Writing bin response:",
                                   reinterpret_cast<const char*>(wbuf.data()),
                                   wbuf.size()) != -1) {
            LOG_DEBUG(&connection, "%s", buffer);
        }
    }

    ++connection.getBucket().responseCounters[err];
    connection.addIov(wbuf.data(), wbuf.size());
}

bool mcbp_response_handler(const void* key, uint16_t keylen,
                           const void* ext, uint8_t extlen,
                           const void* body, uint32_t bodylen,
                           protocol_binary_datatype_t datatype, uint16_t status,
                           uint64_t cas, const void* void_cookie)
{
    auto* ccookie = reinterpret_cast<const Cookie*>(void_cookie);
    auto* cookie = const_cast<Cookie*>(ccookie);
    cookie->validate();

    McbpConnection* c = &cookie->getConnection();
    cb::compression::Buffer buffer;
    cb::const_char_buffer payload(static_cast<const char*>(body), bodylen);

    if (!c->isSnappyEnabled() && mcbp::datatype::is_snappy(datatype)) {
        // The client is not snappy-aware, and the content contains
        // snappy encoded data.. We need to inflate it!
        if (!cb::compression::inflate(cb::compression::Algorithm::Snappy,
                                      payload, buffer)) {
            std::string mykey(reinterpret_cast<const char*>(key), keylen);
            LOG_WARNING(c,
                        "<%u ERROR: Failed to inflate body, "
                            "Key: %s may have an incorrect datatype, "
                            "Datatype indicates that document is %s",
                        c->getId(), mykey.c_str(),
                        mcbp::datatype::to_string(datatype).c_str());
            return false;
        }
        payload = buffer;
        datatype &= ~(PROTOCOL_BINARY_DATATYPE_SNAPPY);
    }

    if (mcbp::datatype::is_xattr(datatype)) {
        // We need to strip off the xattrs
        payload = cb::xattr::get_body(payload);
        datatype &= ~(PROTOCOL_BINARY_DATATYPE_XATTR);
    }

    datatype = c->getEnabledDatatypes(datatype);
    auto& error_json = cookie->getErrorJson();

    switch (status) {
    case PROTOCOL_BINARY_RESPONSE_SUCCESS:
    case PROTOCOL_BINARY_RESPONSE_SUBDOC_SUCCESS_DELETED:
    case PROTOCOL_BINARY_RESPONSE_SUBDOC_MULTI_PATH_FAILURE:
    case PROTOCOL_BINARY_RESPONSE_ROLLBACK:
        break;
    case PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET:
        cookie->sendNotMyVBucket();
        return true;
    default:
        //
        payload = {error_json.data(), error_json.size()};
        keylen = 0;
        extlen = 0;
        datatype = payload.empty() ? PROTOCOL_BINARY_RAW_BYTES
                                   : PROTOCOL_BINARY_DATATYPE_JSON;
    }

    const size_t needed = payload.len + keylen + extlen +
                          sizeof(protocol_binary_response_header);

    auto& dbuf = cookie->getDynamicBuffer();
    if (!dbuf.grow(needed)) {
        LOG_WARNING(c, "<%u ERROR: Failed to allocate memory for response",
                    c->getId());
        return false;
    }

    auto* buf = reinterpret_cast<uint8_t*>(dbuf.getCurrent());
    const auto& header = cookie->getHeader();

    cb::mcbp::ResponseBuilder builder({buf, needed});
    builder.setMagic(cb::mcbp::Magic::ClientResponse);
    builder.setOpcode(header.getRequest().getClientOpcode());
    builder.setDatatype(cb::mcbp::Datatype(datatype));
    builder.setStatus(cb::mcbp::Status(status));
    builder.setExtras({static_cast<const uint8_t*>(ext), extlen});
    builder.setKey({static_cast<const uint8_t*>(key), keylen});
    builder.setValue(
            {reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
    builder.setOpaque(header.getOpaque());
    builder.setCas(cas);
    builder.validate();

    ++c->getBucket().responseCounters[status];
    dbuf.moveOffset(needed);
    return true;
}

void mcbp_collect_timings(Cookie& cookie) {
    auto* c = &cookie.getConnection();
    if (c->isDCP()) {
        // The state machinery works differently for the DCP connections
        // so these timings isn't accurate!
        //
        // For now disable the timings, and add them back once they're
        // correct
        return;
    }
    const auto opcode = cookie.getHeader().getOpcode();
    const auto elapsed_ns = ProcessClock::now() - cookie.getStart();
    // aggregated timing for all buckets
    all_buckets[0].timings.collect(opcode, elapsed_ns);

    // timing for current bucket
    const auto bucketid = c->getBucketIndex();
    /* bucketid will be zero initially before you run sasl auth
     * (unless there is a default bucket), or if someone tries
     * to delete the bucket you're associated with and your're idle.
     */
    if (bucketid != 0) {
        all_buckets[bucketid].timings.collect(opcode, elapsed_ns);
    }

    // Log operations taking longer than 0.5s
    const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_ns);
    cookie.maybeLogSlowCommand(elapsed_ms);
}
