// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/core/quic_framer.h"

#include <cstdint>
#include <memory>

#include "base/compiler_specific.h"
#include "net/quic/core/crypto/crypto_framer.h"
#include "net/quic/core/crypto/crypto_handshake_message.h"
#include "net/quic/core/crypto/crypto_protocol.h"
#include "net/quic/core/crypto/null_decrypter.h"
#include "net/quic/core/crypto/null_encrypter.h"
#include "net/quic/core/crypto/quic_decrypter.h"
#include "net/quic/core/crypto/quic_encrypter.h"
#include "net/quic/core/quic_data_reader.h"
#include "net/quic/core/quic_data_writer.h"
#include "net/quic/core/quic_socket_address_coder.h"
#include "net/quic/core/quic_stream_frame_data_producer.h"
#include "net/quic/core/quic_utils.h"
#include "net/quic/platform/api/quic_aligned.h"
#include "net/quic/platform/api/quic_bug_tracker.h"
#include "net/quic/platform/api/quic_fallthrough.h"
#include "net/quic/platform/api/quic_flag_utils.h"
#include "net/quic/platform/api/quic_flags.h"
#include "net/quic/platform/api/quic_logging.h"
#include "net/quic/platform/api/quic_map_util.h"
#include "net/quic/platform/api/quic_ptr_util.h"
#include "net/quic/platform/api/quic_str_cat.h"
#include "net/quic/platform/api/quic_string.h"
#include "net/quic/platform/api/quic_text_utils.h"


namespace net {

namespace {

#define ENDPOINT \
  (perspective_ == Perspective::IS_SERVER ? "Server: " : "Client: ")

// How much to shift the timestamp in the IETF Ack frame.
// TODO(fkastenholz) when we get real IETF QUIC, need to get
// the currect shift from the transport parameters.
const int kIetfAckTimestampShift = 3;

// Number of bits the packet number length bits are shifted from the right
// edge of the header.
const uint8_t kPublicHeaderSequenceNumberShift = 4;

// New Frame Types, QUIC v. >= 10:
// There are two interpretations for the Frame Type byte in the QUIC protocol,
// resulting in two Frame Types: Special Frame Types and Regular Frame Types.
//
// Regular Frame Types use the Frame Type byte simply. Currently defined
// Regular Frame Types are:
// Padding            : 0b 00000000 (0x00)
// ResetStream        : 0b 00000001 (0x01)
// ConnectionClose    : 0b 00000010 (0x02)
// GoAway             : 0b 00000011 (0x03)
// WindowUpdate       : 0b 00000100 (0x04)
// Blocked            : 0b 00000101 (0x05)
//
// Special Frame Types encode both a Frame Type and corresponding flags
// all in the Frame Type byte. Currently defined Special Frame Types are:
// Stream             : 0b 11xxxxxx
// Ack                : 0b 101xxxxx
//
// Semantics of the flag bits above (the x bits) depends on the frame type.

// Masks to determine if the frame type is a special use
// and for specific special frame types.
const uint8_t kQuicFrameTypeSpecialMask = 0b11100000;
const uint8_t kQuicFrameTypeStreamMask_Pre40 = 0b10000000;
const uint8_t kQuicFrameTypeStreamMask = 0b11000000;
const uint8_t kQuicFrameTypeAckMask_Pre40 = 0b01000000;
const uint8_t kQuicFrameTypeAckMask = 0b10100000;

// Stream type format is 11FSSOOD.
// Stream frame relative shifts and masks for interpreting the stream flags.
// StreamID may be 1, 2, 3, or 4 bytes.
const uint8_t kQuicStreamIdShift_Pre40 = 2;
const uint8_t kQuicStreamIDLengthMask_Pre40 = 0b00000011;
const uint8_t kQuicStreamIDLengthShift = 3;
const uint8_t kQuicStreamIDLengthNumBits = 2;

// Offset may be 0, 2, 4, or 8 bytes.
const uint8_t kQuicStreamShift_Pre40 = 3;
const uint8_t kQuicStreamOffsetMask_Pre40 = 0b00000111;
const uint8_t kQuicStreamOffsetNumBits = 2;
const uint8_t kQuicStreamOffsetShift = 1;

// Data length may be 0 or 2 bytes.
const uint8_t kQuicStreamDataLengthShift_Pre40 = 1;
const uint8_t kQuicStreamDataLengthMask_Pre40 = 0b00000001;
const uint8_t kQuicStreamDataLengthShift = 0;

// Fin bit may be set or not.
const uint8_t kQuicStreamFinShift_Pre40 = 1;
const uint8_t kQuicStreamFinMask_Pre40 = 0b00000001;
const uint8_t kQuicStreamFinShift = 5;

// packet number size shift used in AckFrames.
const uint8_t kQuicSequenceNumberLengthNumBits = 2;
const uint8_t kActBlockLengthOffset = 0;
const uint8_t kLargestAckedOffset = 2;

// Acks may have only one ack block.
const uint8_t kQuicHasMultipleAckBlocksOffset_Pre40 = 5;
const uint8_t kQuicHasMultipleAckBlocksOffset = 4;

// Maximum length of encoded error strings.
const int kMaxErrorStringLength = 256;

// Returns the absolute value of the difference between |a| and |b|.
QuicPacketNumber Delta(QuicPacketNumber a, QuicPacketNumber b) {
  // Since these are unsigned numbers, we can't just return abs(a - b)
  if (a < b) {
    return b - a;
  }
  return a - b;
}

QuicPacketNumber ClosestTo(QuicPacketNumber target,
                           QuicPacketNumber a,
                           QuicPacketNumber b) {
  return (Delta(target, a) < Delta(target, b)) ? a : b;
}

QuicPacketNumberLength ReadSequenceNumberLength(uint8_t flags) {
  switch (flags & PACKET_FLAGS_8BYTE_PACKET) {
    case PACKET_FLAGS_8BYTE_PACKET:
      return PACKET_6BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_4BYTE_PACKET:
      return PACKET_4BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_2BYTE_PACKET:
      return PACKET_2BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_1BYTE_PACKET:
      return PACKET_1BYTE_PACKET_NUMBER;
    default:
      QUIC_BUG << "Unreachable case statement.";
      return PACKET_6BYTE_PACKET_NUMBER;
  }
}

QuicPacketNumberLength ReadAckPacketNumberLength(QuicTransportVersion version,
                                                 uint8_t flags) {
  switch (flags & PACKET_FLAGS_8BYTE_PACKET) {
    case PACKET_FLAGS_8BYTE_PACKET:
      return version != QUIC_VERSION_41 ? PACKET_6BYTE_PACKET_NUMBER
                                        : PACKET_8BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_4BYTE_PACKET:
      return PACKET_4BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_2BYTE_PACKET:
      return PACKET_2BYTE_PACKET_NUMBER;
    case PACKET_FLAGS_1BYTE_PACKET:
      return PACKET_1BYTE_PACKET_NUMBER;
    default:
      QUIC_BUG << "Unreachable case statement.";
      return PACKET_6BYTE_PACKET_NUMBER;
  }
}

}  // namespace

QuicFramer::QuicFramer(const ParsedQuicVersionVector& supported_versions,
                       QuicTime creation_time,
                       Perspective perspective)
    : visitor_(nullptr),
      error_(QUIC_NO_ERROR),
      largest_packet_number_(0),
      last_serialized_connection_id_(0),
      last_version_label_(0),
      version_(PROTOCOL_UNSUPPORTED, QUIC_VERSION_UNSUPPORTED),
      supported_versions_(supported_versions),
      decrypter_level_(ENCRYPTION_NONE),
      alternative_decrypter_level_(ENCRYPTION_NONE),
      alternative_decrypter_latch_(false),
      perspective_(perspective),
      validate_flags_(true),
      creation_time_(creation_time),
      last_timestamp_(QuicTime::Delta::Zero()),
      data_producer_(nullptr),
      use_incremental_ack_processing_(
          GetQuicReloadableFlag(quic_use_incremental_ack_processing2)) {
  if (use_incremental_ack_processing_) {
    QUIC_FLAG_COUNT(quic_reloadable_flag_quic_use_incremental_ack_processing2);
  }
  DCHECK(!supported_versions.empty());
  version_ = supported_versions_[0];
  decrypter_ = QuicMakeUnique<NullDecrypter>(perspective);
  encrypter_[ENCRYPTION_NONE] = QuicMakeUnique<NullEncrypter>(perspective);
}

QuicFramer::~QuicFramer() {}

// static
size_t QuicFramer::GetMinStreamFrameSize(QuicTransportVersion version,
                                         QuicStreamId stream_id,
                                         QuicStreamOffset offset,
                                         bool last_frame_in_packet) {
  return kQuicFrameTypeSize + GetStreamIdSize(stream_id) +
         GetStreamOffsetSize(version, offset) +
         (last_frame_in_packet ? 0 : kQuicStreamPayloadLengthSize);
}

// static
size_t QuicFramer::GetMinAckFrameSize(
    QuicTransportVersion version,
    QuicPacketNumberLength largest_observed_length) {
  size_t min_size = kQuicFrameTypeSize + largest_observed_length +
                    kQuicDeltaTimeLargestObservedSize;
  return min_size + kQuicNumTimestampsSize;
}

// static
size_t QuicFramer::GetStopWaitingFrameSize(
    QuicTransportVersion version,
    QuicPacketNumberLength packet_number_length) {
  size_t min_size = kQuicFrameTypeSize + packet_number_length;
  return min_size;
}

// static
size_t QuicFramer::GetRstStreamFrameSize() {
  return kQuicFrameTypeSize + kQuicMaxStreamIdSize + kQuicMaxStreamOffsetSize +
         kQuicErrorCodeSize;
}

// static
size_t QuicFramer::GetMinConnectionCloseFrameSize() {
  return kQuicFrameTypeSize + kQuicErrorCodeSize + kQuicErrorDetailsLengthSize;
}

// static
size_t QuicFramer::GetMinGoAwayFrameSize() {
  return kQuicFrameTypeSize + kQuicErrorCodeSize + kQuicErrorDetailsLengthSize +
         kQuicMaxStreamIdSize;
}

// static
size_t QuicFramer::GetWindowUpdateFrameSize() {
  return kQuicFrameTypeSize + kQuicMaxStreamIdSize + kQuicMaxStreamOffsetSize;
}

// static
size_t QuicFramer::GetBlockedFrameSize() {
  return kQuicFrameTypeSize + kQuicMaxStreamIdSize;
}

// static
size_t QuicFramer::GetStreamIdSize(QuicStreamId stream_id) {
  // Sizes are 1 through 4 bytes.
  for (int i = 1; i <= 4; ++i) {
    stream_id >>= 8;
    if (stream_id == 0) {
      return i;
    }
  }
  QUIC_BUG << "Failed to determine StreamIDSize.";
  return 4;
}

// static
size_t QuicFramer::GetStreamOffsetSize(QuicTransportVersion version,
                                       QuicStreamOffset offset) {
  if (version != QUIC_VERSION_41) {
    // 0 is a special case.
    if (offset == 0) {
      return 0;
    }
    // 2 through 8 are the remaining sizes.
    offset >>= 8;
    for (int i = 2; i <= 8; ++i) {
      offset >>= 8;
      if (offset == 0) {
        return i;
      }
    }
    QUIC_BUG << "Failed to determine StreamOffsetSize.";
    return 8;
  }
  // try 0, 2 and 4.
  for (int i = 0; i <= 4; i += 2) {
    if ((offset >> (8 * i)) == 0) {
      return i;
    }
  }

  // 8 is the only remaining valid value and will contain any 64bit offset.
  return 8;
}

// static
size_t QuicFramer::GetVersionNegotiationPacketSize(size_t number_versions) {
  return kPublicFlagsSize + PACKET_8BYTE_CONNECTION_ID +
         number_versions * kQuicVersionSize;
}

// TODO(nharper): Change this method to take a ParsedQuicVersion.
bool QuicFramer::IsSupportedTransportVersion(
    const QuicTransportVersion version) const {
  for (ParsedQuicVersion supported_version : supported_versions_) {
    if (version == supported_version.transport_version) {
      return true;
    }
  }
  return false;
}

bool QuicFramer::IsSupportedVersion(const ParsedQuicVersion version) const {
  for (const ParsedQuicVersion& supported_version : supported_versions_) {
    if (version == supported_version) {
      return true;
    }
  }
  return false;
}

size_t QuicFramer::GetSerializedFrameLength(
    const QuicFrame& frame,
    size_t free_bytes,
    bool first_frame,
    bool last_frame,
    QuicPacketNumberLength packet_number_length) {
  // Prevent a rare crash reported in b/19458523.
  if ((frame.type == STREAM_FRAME || frame.type == ACK_FRAME) &&
      frame.stream_frame == nullptr) {
    QUIC_BUG << "Cannot compute the length of a null frame. "
             << "type:" << frame.type << "free_bytes:" << free_bytes
             << " first_frame:" << first_frame << " last_frame:" << last_frame
             << " seq num length:" << packet_number_length;
    RecordInternalErrorLocation(QUIC_FRAMER);
    set_error(QUIC_INTERNAL_ERROR);
    visitor_->OnError(this);
    return 0;
  }
  if (frame.type == PADDING_FRAME) {
    if (frame.padding_frame.num_padding_bytes == -1) {
      // Full padding to the end of the packet.
      return free_bytes;
    } else {
      // Lite padding.
      return free_bytes <
                     static_cast<size_t>(frame.padding_frame.num_padding_bytes)
                 ? free_bytes
                 : frame.padding_frame.num_padding_bytes;
    }
  }

  size_t frame_len =
      ComputeFrameLength(frame, last_frame, packet_number_length);
  if (frame_len <= free_bytes) {
    // Frame fits within packet. Note that acks may be truncated.
    return frame_len;
  }
  // Only truncate the first frame in a packet, so if subsequent ones go
  // over, stop including more frames.
  if (!first_frame) {
    return 0;
  }
  bool can_truncate =
      frame.type == ACK_FRAME &&
      free_bytes >= GetMinAckFrameSize(version_.transport_version,
                                       PACKET_6BYTE_PACKET_NUMBER);
  if (can_truncate) {
    // Truncate the frame so the packet will not exceed kMaxPacketSize.
    // Note that we may not use every byte of the writer in this case.
    QUIC_DLOG(INFO) << ENDPOINT
                    << "Truncating large frame, free bytes: " << free_bytes;
    return free_bytes;
  }
  return 0;
}

QuicFramer::AckFrameInfo::AckFrameInfo()
    : max_block_length(0), first_block_length(0), num_ack_blocks(0) {}

QuicFramer::AckFrameInfo::AckFrameInfo(const AckFrameInfo& other) = default;

QuicFramer::AckFrameInfo::~AckFrameInfo() {}

size_t QuicFramer::BuildDataPacket(const QuicPacketHeader& header,
                                   const QuicFrames& frames,
                                   char* buffer,
                                   size_t packet_length) {
  QuicDataWriter writer(packet_length, buffer, endianness());
  if (!AppendPacketHeader(header, &writer)) {
    QUIC_BUG << "AppendPacketHeader failed";
    return 0;
  }

  size_t i = 0;
  for (const QuicFrame& frame : frames) {
    // Determine if we should write stream frame length in header.
    const bool no_stream_frame_length = i == frames.size() - 1;
    if (!AppendTypeByte(frame, no_stream_frame_length, &writer)) {
      QUIC_BUG << "AppendTypeByte failed";
      return 0;
    }

    switch (frame.type) {
      case PADDING_FRAME:
        if (!AppendPaddingFrame(frame.padding_frame, &writer)) {
          QUIC_BUG << "AppendPaddingFrame of "
                   << frame.padding_frame.num_padding_bytes << " failed";
          return 0;
        }
        break;
      case STREAM_FRAME:
        if (!AppendStreamFrame(*frame.stream_frame, no_stream_frame_length,
                               &writer)) {
          QUIC_BUG << "AppendStreamFrame failed";
          return 0;
        }
        break;
      case ACK_FRAME:
        if (!AppendAckFrameAndTypeByte(*frame.ack_frame, &writer)) {
          QUIC_BUG << "AppendAckFrameAndTypeByte failed";
          return 0;
        }
        break;
      case STOP_WAITING_FRAME:
        if (!AppendStopWaitingFrame(header, *frame.stop_waiting_frame,
                                    &writer)) {
          QUIC_BUG << "AppendStopWaitingFrame failed";
          return 0;
        }
        break;
      case MTU_DISCOVERY_FRAME:
        // MTU discovery frames are serialized as ping frames.
        QUIC_FALLTHROUGH_INTENDED;
      case PING_FRAME:
        // Ping has no payload.
        break;
      case RST_STREAM_FRAME:
        if (!AppendRstStreamFrame(*frame.rst_stream_frame, &writer)) {
          QUIC_BUG << "AppendRstStreamFrame failed";
          return 0;
        }
        break;
      case CONNECTION_CLOSE_FRAME:
        if (!AppendConnectionCloseFrame(*frame.connection_close_frame,
                                        &writer)) {
          QUIC_BUG << "AppendConnectionCloseFrame failed";
          return 0;
        }
        break;
      case GOAWAY_FRAME:
        if (!AppendGoAwayFrame(*frame.goaway_frame, &writer)) {
          QUIC_BUG << "AppendGoAwayFrame failed";
          return 0;
        }
        break;
      case WINDOW_UPDATE_FRAME:
        if (!AppendWindowUpdateFrame(*frame.window_update_frame, &writer)) {
          QUIC_BUG << "AppendWindowUpdateFrame failed";
          return 0;
        }
        break;
      case BLOCKED_FRAME:
        if (!AppendBlockedFrame(*frame.blocked_frame, &writer)) {
          QUIC_BUG << "AppendBlockedFrame failed";
          return 0;
        }
        break;
      default:
        RaiseError(QUIC_INVALID_FRAME_DATA);
        QUIC_BUG << "QUIC_INVALID_FRAME_DATA";
        return 0;
    }
    ++i;
  }

  return writer.length();
}

size_t QuicFramer::BuildConnectivityProbingPacket(
    const QuicPacketHeader& header,
    char* buffer,
    size_t packet_length) {
  QuicDataWriter writer(packet_length, buffer, endianness());

  if (!AppendPacketHeader(header, &writer)) {
    QUIC_BUG << "AppendPacketHeader failed";
    return 0;
  }

  // Write a PING frame, which has no data payload.
  QuicPingFrame ping_frame;
  if (!AppendTypeByte(QuicFrame(ping_frame), false, &writer)) {
    QUIC_BUG << "AppendTypeByte failed for ping frame in probing packet";
    return 0;
  }

  // Add padding to the rest of the packet.
  QuicPaddingFrame padding_frame;
  if (!AppendTypeByte(QuicFrame(padding_frame), true, &writer)) {
    QUIC_BUG << "AppendTypeByte failed for padding frame in probing packet";
    return 0;
  }
  if (!AppendPaddingFrame(padding_frame, &writer)) {
    QUIC_BUG << "AppendPaddingFrame of " << padding_frame.num_padding_bytes
             << " failed";
    return 0;
  }

  return writer.length();
}

// static
std::unique_ptr<QuicEncryptedPacket> QuicFramer::BuildPublicResetPacket(
    const QuicPublicResetPacket& packet) {
  CryptoHandshakeMessage reset;
  reset.set_tag(kPRST);
  reset.SetValue(kRNON, packet.nonce_proof);
  if (packet.client_address.host().address_family() !=
      IpAddressFamily::IP_UNSPEC) {
    // packet.client_address is non-empty.
    QuicSocketAddressCoder address_coder(packet.client_address);
    QuicString serialized_address = address_coder.Encode();
    if (serialized_address.empty()) {
      return nullptr;
    }
    reset.SetStringPiece(kCADR, serialized_address);
  }
  const QuicData& reset_serialized =
      reset.GetSerialized(Perspective::IS_SERVER);

  size_t len =
      kPublicFlagsSize + PACKET_8BYTE_CONNECTION_ID + reset_serialized.length();
  std::unique_ptr<char[]> buffer(new char[len]);
  // Endianness is not a concern here, as writer is not going to write integers
  // or floating numbers.
  QuicDataWriter writer(len, buffer.get(), NETWORK_BYTE_ORDER);

  uint8_t flags = static_cast<uint8_t>(PACKET_PUBLIC_FLAGS_RST |
                                       PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID);
  // This hack makes post-v33 public reset packet look like pre-v33 packets.
  flags |= static_cast<uint8_t>(PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID_OLD);
  if (!writer.WriteUInt8(flags)) {
    return nullptr;
  }

  if (!writer.WriteConnectionId(packet.connection_id)) {
    return nullptr;
  }

  if (!writer.WriteBytes(reset_serialized.data(), reset_serialized.length())) {
    return nullptr;
  }

  return QuicMakeUnique<QuicEncryptedPacket>(buffer.release(), len, true);
}

// static
std::unique_ptr<QuicEncryptedPacket> QuicFramer::BuildVersionNegotiationPacket(
    QuicConnectionId connection_id,
    const ParsedQuicVersionVector& versions) {
  DCHECK(!versions.empty());
  size_t len = GetVersionNegotiationPacketSize(versions.size());
  std::unique_ptr<char[]> buffer(new char[len]);
  // Endianness is not a concern here, version negotiation packet does not have
  // integers or floating numbers.
  QuicDataWriter writer(len, buffer.get(), NETWORK_BYTE_ORDER);

  uint8_t flags = static_cast<uint8_t>(
      PACKET_PUBLIC_FLAGS_VERSION | PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID |
      // TODO(rch): Remove this QUIC_VERSION_32 is retired.
      PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID_OLD);
  if (!writer.WriteUInt8(flags)) {
    return nullptr;
  }

  if (!writer.WriteConnectionId(connection_id)) {
    return nullptr;
  }

  for (const ParsedQuicVersion& version : versions) {
    // TODO(rch): Use WriteUInt32() once QUIC_VERSION_38 and earlier
    // are removed.
    if (!writer.WriteTag(
            QuicEndian::HostToNet32(CreateQuicVersionLabel(version)))) {
      return nullptr;
    }
  }

  return QuicMakeUnique<QuicEncryptedPacket>(buffer.release(), len, true);
}

bool QuicFramer::ProcessPacket(const QuicEncryptedPacket& packet) {
  QuicDataReader reader(packet.data(), packet.length(), endianness());

  visitor_->OnPacket();

  QuicPacketHeader header;
  if (!ProcessPublicHeader(&reader, &header)) {
    DCHECK_NE("", detailed_error_);
    QUIC_DVLOG(1) << ENDPOINT << "Unable to process public header. Error: "
                  << detailed_error_;
    DCHECK_NE("", detailed_error_);
    return RaiseError(QUIC_INVALID_PACKET_HEADER);
  }

  if (!visitor_->OnUnauthenticatedPublicHeader(header)) {
    // The visitor suppresses further processing of the packet.
    return true;
  }

  if (perspective_ == Perspective::IS_SERVER && header.version_flag &&
      header.version != version_) {
    if (!visitor_->OnProtocolVersionMismatch(header.version)) {
      return true;
    }
  }

  // framer's version may change, reset reader's endianness.
  reader.set_endianness(endianness());

  bool rv;
  if (perspective_ == Perspective::IS_CLIENT && header.version_flag) {
    rv = ProcessVersionNegotiationPacket(&reader, header);
  } else if (header.reset_flag) {
    rv = ProcessPublicResetPacket(&reader, header);
  } else if (packet.length() <= kMaxPacketSize) {
    // The optimized decryption algorithm implementations run faster when
    // operating on aligned memory.
    QUIC_CACHELINE_ALIGNED char buffer[kMaxPacketSize];
    rv = ProcessDataPacket(&reader, &header, packet, buffer, kMaxPacketSize);
  } else {
    std::unique_ptr<char[]> large_buffer(new char[packet.length()]);
    rv = ProcessDataPacket(&reader, &header, packet, large_buffer.get(),
                           packet.length());
    QUIC_BUG_IF(rv) << "QUIC should never successfully process packets larger"
                    << "than kMaxPacketSize. packet size:" << packet.length();
  }

  return rv;
}

bool QuicFramer::ProcessVersionNegotiationPacket(
    QuicDataReader* reader,
    const QuicPacketHeader& header) {
  DCHECK_EQ(Perspective::IS_CLIENT, perspective_);

  QuicVersionNegotiationPacket packet(header.connection_id);
  // Try reading at least once to raise error if the packet is invalid.
  do {
    QuicVersionLabel version_label;
    if (!reader->ReadTag(&version_label)) {
      set_detailed_error("Unable to read supported version in negotiation.");
      return RaiseError(QUIC_INVALID_VERSION_NEGOTIATION_PACKET);
    }
    // TODO(rch): Use ReadUInt32() once QUIC_VERSION_38 and earlier
    // are removed.
    version_label = QuicEndian::NetToHost32(version_label);
    packet.versions.push_back(ParseQuicVersionLabel(version_label));
  } while (!reader->IsDoneReading());

  visitor_->OnVersionNegotiationPacket(packet);
  return true;
}

bool QuicFramer::ProcessDataPacket(QuicDataReader* encrypted_reader,
                                   QuicPacketHeader* header,
                                   const QuicEncryptedPacket& packet,
                                   char* decrypted_buffer,
                                   size_t buffer_length) {
  if (!ProcessUnauthenticatedHeader(encrypted_reader, header)) {
    DCHECK_NE("", detailed_error_);
    QUIC_DVLOG(1)
        << ENDPOINT
        << "Unable to process packet header. Stopping parsing. Error: "
        << detailed_error_;
    return false;
  }

  size_t decrypted_length = 0;
  if (!DecryptPayload(encrypted_reader, *header, packet, decrypted_buffer,
                      buffer_length, &decrypted_length)) {
    set_detailed_error("Unable to decrypt payload.");
    return RaiseError(QUIC_DECRYPTION_FAILURE);
  }

  QuicDataReader reader(decrypted_buffer, decrypted_length, endianness());

  // Update the largest packet number after we have decrypted the packet
  // so we are confident is not attacker controlled.
  largest_packet_number_ =
      std::max(header->packet_number, largest_packet_number_);

  if (!visitor_->OnPacketHeader(*header)) {
    // The visitor suppresses further processing of the packet.
    return true;
  }

  if (packet.length() > kMaxPacketSize) {
    // If the packet has gotten this far, it should not be too large.
    QUIC_BUG << "Packet too large:" << packet.length();
    return RaiseError(QUIC_PACKET_TOO_LARGE);
  }

  // Handle the payload.
  if (!ProcessFrameData(&reader, *header)) {
    DCHECK_NE(QUIC_NO_ERROR, error_);  // ProcessFrameData sets the error.
    DCHECK_NE("", detailed_error_);
    QUIC_DLOG(WARNING) << ENDPOINT << "Unable to process frame data. Error: "
                       << detailed_error_;
    return false;
  }

  visitor_->OnPacketComplete();
  return true;
}

bool QuicFramer::ProcessPublicResetPacket(QuicDataReader* reader,
                                          const QuicPacketHeader& header) {
  QuicPublicResetPacket packet(header.connection_id);

  std::unique_ptr<CryptoHandshakeMessage> reset(
      CryptoFramer::ParseMessage(reader->ReadRemainingPayload(), perspective_));
  if (!reset.get()) {
    set_detailed_error("Unable to read reset message.");
    return RaiseError(QUIC_INVALID_PUBLIC_RST_PACKET);
  }
  if (reset->tag() != kPRST) {
    set_detailed_error("Incorrect message tag.");
    return RaiseError(QUIC_INVALID_PUBLIC_RST_PACKET);
  }

  if (reset->GetUint64(kRNON, &packet.nonce_proof) != QUIC_NO_ERROR) {
    set_detailed_error("Unable to read nonce proof.");
    return RaiseError(QUIC_INVALID_PUBLIC_RST_PACKET);
  }
  // TODO(satyamshekhar): validate nonce to protect against DoS.

  QuicStringPiece address;
  if (reset->GetStringPiece(kCADR, &address)) {
    QuicSocketAddressCoder address_coder;
    if (address_coder.Decode(address.data(), address.length())) {
      packet.client_address =
          QuicSocketAddress(address_coder.ip(), address_coder.port());
    }
  }

  visitor_->OnPublicResetPacket(packet);
  return true;
}

bool QuicFramer::AppendPacketHeader(const QuicPacketHeader& header,
                                    QuicDataWriter* writer) {
  QUIC_DVLOG(1) << ENDPOINT << "Appending header: " << header;
  uint8_t public_flags = 0;
  if (header.reset_flag) {
    public_flags |= PACKET_PUBLIC_FLAGS_RST;
  }
  if (header.version_flag) {
    public_flags |= PACKET_PUBLIC_FLAGS_VERSION;
  }

  public_flags |= GetPacketNumberFlags(header.packet_number_length)
                  << kPublicHeaderSequenceNumberShift;

  if (header.nonce != nullptr) {
    DCHECK_EQ(Perspective::IS_SERVER, perspective_);
    public_flags |= PACKET_PUBLIC_FLAGS_NONCE;
  }

  switch (header.connection_id_length) {
    case PACKET_0BYTE_CONNECTION_ID:
      if (!writer->WriteUInt8(public_flags |
                              PACKET_PUBLIC_FLAGS_0BYTE_CONNECTION_ID)) {
        return false;
      }
      break;
    case PACKET_8BYTE_CONNECTION_ID:
      public_flags |= PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID;
      if (perspective_ == Perspective::IS_CLIENT) {
        public_flags |= PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID_OLD;
      }
      if (!writer->WriteUInt8(public_flags) ||
          !writer->WriteConnectionId(header.connection_id)) {
        return false;
      }
      break;
  }
  last_serialized_connection_id_ = header.connection_id;

  if (header.version_flag) {
    DCHECK_EQ(Perspective::IS_CLIENT, perspective_);
    QuicVersionLabel version_label = CreateQuicVersionLabel(version_);
    // TODO(rch): Use WriteUInt32() once QUIC_VERSION_38 and earlier
    // are removed.
    if (!writer->WriteTag(QuicEndian::NetToHost32(version_label))) {
      return false;
    }

    QUIC_DVLOG(1) << ENDPOINT << "label = '"
                  << QuicVersionLabelToString(version_label) << "'";
  }

  if (header.nonce != nullptr &&
      !writer->WriteBytes(header.nonce, kDiversificationNonceSize)) {
    return false;
  }

  if (!AppendPacketNumber(header.packet_number_length, header.packet_number,
                          writer)) {
    return false;
  }

  return true;
}

const QuicTime::Delta QuicFramer::CalculateTimestampFromWire(
    uint32_t time_delta_us) {
  // The new time_delta might have wrapped to the next epoch, or it
  // might have reverse wrapped to the previous epoch, or it might
  // remain in the same epoch. Select the time closest to the previous
  // time.
  //
  // epoch_delta is the delta between epochs. A delta is 4 bytes of
  // microseconds.
  const uint64_t epoch_delta = UINT64_C(1) << 32;
  uint64_t epoch = last_timestamp_.ToMicroseconds() & ~(epoch_delta - 1);
  // Wrapping is safe here because a wrapped value will not be ClosestTo below.
  uint64_t prev_epoch = epoch - epoch_delta;
  uint64_t next_epoch = epoch + epoch_delta;

  uint64_t time = ClosestTo(
      last_timestamp_.ToMicroseconds(), epoch + time_delta_us,
      ClosestTo(last_timestamp_.ToMicroseconds(), prev_epoch + time_delta_us,
                next_epoch + time_delta_us));

  return QuicTime::Delta::FromMicroseconds(time);
}

QuicPacketNumber QuicFramer::CalculatePacketNumberFromWire(
    QuicPacketNumberLength packet_number_length,
    QuicPacketNumber base_packet_number,
    QuicPacketNumber packet_number) const {
  // The new packet number might have wrapped to the next epoch, or
  // it might have reverse wrapped to the previous epoch, or it might
  // remain in the same epoch.  Select the packet number closest to the
  // next expected packet number, the previous packet number plus 1.

  // epoch_delta is the delta between epochs the packet number was serialized
  // with, so the correct value is likely the same epoch as the last sequence
  // number or an adjacent epoch.
  const QuicPacketNumber epoch_delta = UINT64_C(1)
                                       << (8 * packet_number_length);
  QuicPacketNumber next_packet_number = base_packet_number + 1;
  QuicPacketNumber epoch = base_packet_number & ~(epoch_delta - 1);
  QuicPacketNumber prev_epoch = epoch - epoch_delta;
  QuicPacketNumber next_epoch = epoch + epoch_delta;

  return ClosestTo(next_packet_number, epoch + packet_number,
                   ClosestTo(next_packet_number, prev_epoch + packet_number,
                             next_epoch + packet_number));
}

bool QuicFramer::ProcessPublicHeader(QuicDataReader* reader,
                                     QuicPacketHeader* header) {
  uint8_t public_flags;
  if (!reader->ReadBytes(&public_flags, 1)) {
    set_detailed_error("Unable to read public flags.");
    return false;
  }

  header->reset_flag = (public_flags & PACKET_PUBLIC_FLAGS_RST) != 0;
  header->version_flag = (public_flags & PACKET_PUBLIC_FLAGS_VERSION) != 0;

  if (validate_flags_ && !header->version_flag &&
      public_flags > PACKET_PUBLIC_FLAGS_MAX) {
    set_detailed_error("Illegal public flags value.");
    return false;
  }

  if (header->reset_flag && header->version_flag) {
    set_detailed_error("Got version flag in reset packet");
    return false;
  }

  switch (public_flags & PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID) {
    case PACKET_PUBLIC_FLAGS_8BYTE_CONNECTION_ID:
      if (!reader->ReadConnectionId(&header->connection_id)) {
        set_detailed_error("Unable to read ConnectionId.");
        return false;
      }
      header->connection_id_length = PACKET_8BYTE_CONNECTION_ID;
      break;
    case PACKET_PUBLIC_FLAGS_0BYTE_CONNECTION_ID:
      header->connection_id_length = PACKET_0BYTE_CONNECTION_ID;
      header->connection_id = last_serialized_connection_id_;
      break;
  }

  header->packet_number_length = ReadSequenceNumberLength(
      public_flags >> kPublicHeaderSequenceNumberShift);

  // Read the version only if the packet is from the client.
  // version flag from the server means version negotiation packet.
  if (header->version_flag && perspective_ == Perspective::IS_SERVER) {
    QuicVersionLabel version_label;
    if (!reader->ReadTag(&version_label)) {
      set_detailed_error("Unable to read protocol version.");
      return false;
    }
    // TODO(rch): Use ReadUInt32() once QUIC_VERSION_38 and earlier
    // are removed.
    version_label = QuicEndian::NetToHost32(version_label);

    // If the version from the new packet is the same as the version of this
    // framer, then the public flags should be set to something we understand.
    // If not, this raises an error.
    last_version_label_ = version_label;
    ParsedQuicVersion version = ParseQuicVersionLabel(version_label);
    if (version == version_ && public_flags > PACKET_PUBLIC_FLAGS_MAX) {
      set_detailed_error("Illegal public flags value.");
      return false;
    }
    header->version = version;
  }

  // A nonce should only be present in packets from the server to the client,
  // which are neither version negotiation nor public reset packets.
  if (public_flags & PACKET_PUBLIC_FLAGS_NONCE &&
      !(public_flags & PACKET_PUBLIC_FLAGS_VERSION) &&
      !(public_flags & PACKET_PUBLIC_FLAGS_RST) &&
      // The nonce flag from a client is ignored and is assumed to be an older
      // client indicating an eight-byte connection ID.
      perspective_ == Perspective::IS_CLIENT) {
    if (!reader->ReadBytes(reinterpret_cast<uint8_t*>(last_nonce_.data()),
                           last_nonce_.size())) {
      set_detailed_error("Unable to read nonce.");
      return false;
    }
    header->nonce = &last_nonce_;
  } else {
    header->nonce = nullptr;
  }

  return true;
}

// static
QuicPacketNumberLength QuicFramer::GetMinPacketNumberLength(
    QuicTransportVersion version,
    QuicPacketNumber packet_number) {
  if (packet_number < 1 << (PACKET_1BYTE_PACKET_NUMBER * 8)) {
    return PACKET_1BYTE_PACKET_NUMBER;
  } else if (packet_number < 1 << (PACKET_2BYTE_PACKET_NUMBER * 8)) {
    return PACKET_2BYTE_PACKET_NUMBER;
  } else if (packet_number < UINT64_C(1) << (PACKET_4BYTE_PACKET_NUMBER * 8)) {
    return PACKET_4BYTE_PACKET_NUMBER;
  } else {
    return version != QUIC_VERSION_41 ? PACKET_6BYTE_PACKET_NUMBER
                                      : PACKET_8BYTE_PACKET_NUMBER;
  }
}

// static
uint8_t QuicFramer::GetPacketNumberFlags(
    QuicPacketNumberLength packet_number_length) {
  switch (packet_number_length) {
    case PACKET_1BYTE_PACKET_NUMBER:
      return PACKET_FLAGS_1BYTE_PACKET;
    case PACKET_2BYTE_PACKET_NUMBER:
      return PACKET_FLAGS_2BYTE_PACKET;
    case PACKET_4BYTE_PACKET_NUMBER:
      return PACKET_FLAGS_4BYTE_PACKET;
    case PACKET_6BYTE_PACKET_NUMBER:
    case PACKET_8BYTE_PACKET_NUMBER:
      return PACKET_FLAGS_8BYTE_PACKET;
    default:
      QUIC_BUG << "Unreachable case statement.";
      return PACKET_FLAGS_8BYTE_PACKET;
  }
}

// static
QuicFramer::AckFrameInfo QuicFramer::GetAckFrameInfo(
    const QuicAckFrame& frame) {
  AckFrameInfo new_ack_info;
  if (frame.packets.Empty()) {
    return new_ack_info;
  }
  // The first block is the last interval. It isn't encoded with the gap-length
  // encoding, so skip it.
  new_ack_info.first_block_length = frame.packets.LastIntervalLength();
  auto itr = frame.packets.rbegin();
  QuicPacketNumber previous_start = itr->min();
  new_ack_info.max_block_length = itr->Length();
  ++itr;

  // Don't do any more work after getting information for 256 ACK blocks; any
  // more can't be encoded anyway.
  for (; itr != frame.packets.rend() &&
         new_ack_info.num_ack_blocks < std::numeric_limits<uint8_t>::max();
       previous_start = itr->min(), ++itr) {
    const auto& interval = *itr;
    const QuicPacketNumber total_gap = previous_start - interval.max();
    new_ack_info.num_ack_blocks +=
        (total_gap + std::numeric_limits<uint8_t>::max() - 1) /
        std::numeric_limits<uint8_t>::max();
    new_ack_info.max_block_length =
        std::max(new_ack_info.max_block_length, interval.Length());
  }
  return new_ack_info;
}

bool QuicFramer::ProcessUnauthenticatedHeader(QuicDataReader* encrypted_reader,
                                              QuicPacketHeader* header) {
  QuicPacketNumber base_packet_number = largest_packet_number_;

  if (!ProcessAndCalculatePacketNumber(
          encrypted_reader, header->packet_number_length, base_packet_number,
          &header->packet_number)) {
    set_detailed_error("Unable to read packet number.");
    return RaiseError(QUIC_INVALID_PACKET_HEADER);
  }

  if (header->packet_number == 0u) {
    set_detailed_error("packet numbers cannot be 0.");
    return RaiseError(QUIC_INVALID_PACKET_HEADER);
  }

  if (!visitor_->OnUnauthenticatedHeader(*header)) {
    set_detailed_error(
        "Visitor asked to stop processing of unauthenticated header.");
    return false;
  }
  return true;
}

bool QuicFramer::ProcessAndCalculatePacketNumber(
    QuicDataReader* reader,
    QuicPacketNumberLength packet_number_length,
    QuicPacketNumber base_packet_number,
    QuicPacketNumber* packet_number) {
  QuicPacketNumber wire_packet_number;
  if (!reader->ReadBytesToUInt64(packet_number_length, &wire_packet_number)) {
    return false;
  }

  // TODO(ianswett): Explore the usefulness of trying multiple packet numbers
  // in case the first guess is incorrect.
  *packet_number = CalculatePacketNumberFromWire(
      packet_number_length, base_packet_number, wire_packet_number);
  return true;
}

bool QuicFramer::ProcessFrameData(QuicDataReader* reader,
                                  const QuicPacketHeader& header) {
  if (reader->IsDoneReading()) {
    set_detailed_error("Packet has no frames.");
    return RaiseError(QUIC_MISSING_PAYLOAD);
  }
  while (!reader->IsDoneReading()) {
    uint8_t frame_type;
    if (!reader->ReadBytes(&frame_type, 1)) {
      set_detailed_error("Unable to read frame type.");
      return RaiseError(QUIC_INVALID_FRAME_DATA);
    }

    if (frame_type & kQuicFrameTypeSpecialMask) {
      // Stream Frame
      if ((version_.transport_version != QUIC_VERSION_41 &&
           (frame_type & kQuicFrameTypeStreamMask_Pre40)) ||
          (version_.transport_version >= QUIC_VERSION_41 &&
           ((frame_type & kQuicFrameTypeStreamMask) ==
            kQuicFrameTypeStreamMask))) {
        QuicStreamFrame frame;
        if (!ProcessStreamFrame(reader, frame_type, &frame)) {
          return RaiseError(QUIC_INVALID_STREAM_DATA);
        }
        if (!visitor_->OnStreamFrame(frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      // Ack Frame
      if ((version_.transport_version != QUIC_VERSION_41 &&
           (frame_type & kQuicFrameTypeAckMask_Pre40)) ||
          (version_.transport_version >= QUIC_VERSION_41 &&
           ((frame_type & kQuicFrameTypeSpecialMask) ==
            kQuicFrameTypeAckMask))) {
        // TODO(fayang): Remove frame when deprecating
        // quic_reloadable_flag_quic_use_incremental_ack_processing2.
        QuicAckFrame frame;
        if (!ProcessAckFrame(reader, frame_type, &frame)) {
          return RaiseError(QUIC_INVALID_ACK_DATA);
        }
        if (!use_incremental_ack_processing_ && !visitor_->OnAckFrame(frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      // This was a special frame type that did not match any
      // of the known ones. Error.
      set_detailed_error("Illegal frame type.");
      QUIC_DLOG(WARNING) << ENDPOINT << "Illegal frame type: "
                         << static_cast<int>(frame_type);
      return RaiseError(QUIC_INVALID_FRAME_DATA);
    }

    switch (frame_type) {
      case PADDING_FRAME: {
        QuicPaddingFrame frame;
        ProcessPaddingFrame(reader, &frame);
        if (!visitor_->OnPaddingFrame(frame)) {
          QUIC_DVLOG(1) << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case RST_STREAM_FRAME: {
        QuicRstStreamFrame frame;
        if (!ProcessRstStreamFrame(reader, &frame)) {
          return RaiseError(QUIC_INVALID_RST_STREAM_DATA);
        }
        if (!visitor_->OnRstStreamFrame(frame)) {
          QUIC_DVLOG(1) << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case CONNECTION_CLOSE_FRAME: {
        QuicConnectionCloseFrame frame;
        if (!ProcessConnectionCloseFrame(reader, &frame)) {
          return RaiseError(QUIC_INVALID_CONNECTION_CLOSE_DATA);
        }

        if (!visitor_->OnConnectionCloseFrame(frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case GOAWAY_FRAME: {
        QuicGoAwayFrame goaway_frame;
        if (!ProcessGoAwayFrame(reader, &goaway_frame)) {
          return RaiseError(QUIC_INVALID_GOAWAY_DATA);
        }
        if (!visitor_->OnGoAwayFrame(goaway_frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case WINDOW_UPDATE_FRAME: {
        QuicWindowUpdateFrame window_update_frame;
        if (!ProcessWindowUpdateFrame(reader, &window_update_frame)) {
          return RaiseError(QUIC_INVALID_WINDOW_UPDATE_DATA);
        }
        if (!visitor_->OnWindowUpdateFrame(window_update_frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case BLOCKED_FRAME: {
        QuicBlockedFrame blocked_frame;
        if (!ProcessBlockedFrame(reader, &blocked_frame)) {
          return RaiseError(QUIC_INVALID_BLOCKED_DATA);
        }
        if (!visitor_->OnBlockedFrame(blocked_frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      case STOP_WAITING_FRAME: {
        QuicStopWaitingFrame stop_waiting_frame;
        if (!ProcessStopWaitingFrame(reader, header, &stop_waiting_frame)) {
          return RaiseError(QUIC_INVALID_STOP_WAITING_DATA);
        }
        if (!visitor_->OnStopWaitingFrame(stop_waiting_frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }
      case PING_FRAME: {
        // Ping has no payload.
        QuicPingFrame ping_frame;
        if (!visitor_->OnPingFrame(ping_frame)) {
          QUIC_DVLOG(1) << ENDPOINT
                        << "Visitor asked to stop further processing.";
          // Returning true since there was no parsing error.
          return true;
        }
        continue;
      }

      default:
        set_detailed_error("Illegal frame type.");
        QUIC_DLOG(WARNING) << ENDPOINT << "Illegal frame type: "
                           << static_cast<int>(frame_type);
        return RaiseError(QUIC_INVALID_FRAME_DATA);
    }
  }

  return true;
}

namespace {
// Create a mask that sets the last |num_bits| to 1 and the rest to 0.
inline uint8_t GetMaskFromNumBits(uint8_t num_bits) {
  return (1u << num_bits) - 1;
}

// Extract |num_bits| from |flags| offset by |offset|.
uint8_t ExtractBits(uint8_t flags, uint8_t num_bits, uint8_t offset) {
  return (flags >> offset) & GetMaskFromNumBits(num_bits);
}

// Extract the bit at position |offset| from |flags| as a bool.
bool ExtractBit(uint8_t flags, uint8_t offset) {
  return ((flags >> offset) & GetMaskFromNumBits(1)) != 0;
}

// Set |num_bits|, offset by |offset| to |val| in |flags|.
void SetBits(uint8_t* flags, uint8_t val, uint8_t num_bits, uint8_t offset) {
  DCHECK_LE(val, GetMaskFromNumBits(num_bits));
  *flags |= val << offset;
}

// Set the bit at position |offset| to |val| in |flags|.
void SetBit(uint8_t* flags, bool val, uint8_t offset) {
  SetBits(flags, val ? 1 : 0, 1, offset);
}
}  // namespace

bool QuicFramer::ProcessStreamFrame(QuicDataReader* reader,
                                    uint8_t frame_type,
                                    QuicStreamFrame* frame) {
  uint8_t stream_flags = frame_type;

  uint8_t stream_id_length = 0;
  uint8_t offset_length = 4;
  bool has_data_length = true;
  if (version_.transport_version != QUIC_VERSION_41) {
    stream_flags &= ~kQuicFrameTypeStreamMask_Pre40;

    // Read from right to left: StreamID, Offset, Data Length, Fin.
    stream_id_length = (stream_flags & kQuicStreamIDLengthMask_Pre40) + 1;
    stream_flags >>= kQuicStreamIdShift_Pre40;

    offset_length = (stream_flags & kQuicStreamOffsetMask_Pre40);
    // There is no encoding for 1 byte, only 0 and 2 through 8.
    if (offset_length > 0) {
      offset_length += 1;
    }
    stream_flags >>= kQuicStreamShift_Pre40;

    has_data_length = (stream_flags & kQuicStreamDataLengthMask_Pre40) ==
                      kQuicStreamDataLengthMask_Pre40;
    stream_flags >>= kQuicStreamDataLengthShift_Pre40;

    frame->fin =
        (stream_flags & kQuicStreamFinMask_Pre40) == kQuicStreamFinShift_Pre40;

  } else {
    stream_flags &= ~kQuicFrameTypeStreamMask;

    stream_id_length = 1 + ExtractBits(stream_flags, kQuicStreamIDLengthNumBits,
                                       kQuicStreamIDLengthShift);

    offset_length = 1 << ExtractBits(stream_flags, kQuicStreamOffsetNumBits,
                                     kQuicStreamOffsetShift);

    if (offset_length == 1) {
      offset_length = 0;
    }

    has_data_length = ExtractBit(stream_flags, kQuicStreamDataLengthShift);

    frame->fin = ExtractBit(stream_flags, kQuicStreamFinShift);
  }

  uint64_t stream_id;
  if (!reader->ReadBytesToUInt64(stream_id_length, &stream_id)) {
    set_detailed_error("Unable to read stream_id.");
    return false;
  }
  frame->stream_id = static_cast<QuicStreamId>(stream_id);

  if (!reader->ReadBytesToUInt64(offset_length, &frame->offset)) {
    set_detailed_error("Unable to read offset.");
    return false;
  }

  // TODO(ianswett): Don't use QuicStringPiece as an intermediary.
  QuicStringPiece data;
  if (has_data_length) {
    if (!reader->ReadStringPiece16(&data)) {
      set_detailed_error("Unable to read frame data.");
      return false;
    }
  } else {
    if (!reader->ReadStringPiece(&data, reader->BytesRemaining())) {
      set_detailed_error("Unable to read frame data.");
      return false;
    }
  }
  frame->data_buffer = data.data();
  frame->data_length = static_cast<uint16_t>(data.length());

  return true;
}

bool QuicFramer::ProcessIetfStreamFrame(QuicDataReader* reader,
                                        uint8_t frame_type,
                                        QuicStreamFrame* frame) {
  // Read stream id from the frame. It's always present.
  QuicIetfStreamId streamid;
  if (!reader->ReadVarInt62(&streamid)) {
    set_detailed_error("Unable to read stream_id.");
    return false;
  }
  if (streamid > 0xffffffff) {
    set_detailed_error("stream_id is too large.");
    return false;
  }
  frame->stream_id = static_cast<QuicStreamId>(streamid);

  // If we have a data offset, read it. If not, set to 0.
  if (frame_type & IETF_STREAM_FRAME_OFF_BIT) {
    QuicStreamOffset offset;
    if (!reader->ReadVarInt62(&offset)) {
      set_detailed_error("Unable to read stream data offset.");
      return false;
    }
    frame->offset = offset;
  } else {
    // no offset in the frame, ensure it's 0 in the Frame.
    frame->offset = 0;
  }

  // If we have a data length, read it. If not, set to 0.
  if (frame_type & IETF_STREAM_FRAME_LEN_BIT) {
    QuicIetfStreamDataLength length;
    if (!reader->ReadVarInt62(&length)) {
      set_detailed_error("Unable to read stream data length.");
      return false;
    }
    if (length > 0xffff) {
      set_detailed_error("stream data offset is too large.");
      return false;
    }
    frame->data_length = length;
  } else {
    // no length in the frame, it is the number of bytes remaining in the
    // packet.
    frame->data_length = reader->BytesRemaining();
  }

  if (frame_type & IETF_STREAM_FRAME_FIN_BIT) {
    frame->fin = true;
  } else {
    frame->fin = false;
  }

  // TODO(ianswett): Don't use QuicStringPiece as an intermediary.
  QuicStringPiece data;
  if (!reader->ReadStringPiece(&data, frame->data_length)) {
    set_detailed_error("Unable to read frame data.");
    return false;
  }
  frame->data_buffer = data.data();
  frame->data_length = static_cast<QuicIetfStreamDataLength>(data.length());

  return true;
}

bool QuicFramer::ProcessAckFrame(QuicDataReader* reader,
                                 uint8_t frame_type,
                                 QuicAckFrame* ack_frame) {
  bool has_ack_blocks =
      ExtractBit(frame_type, version_.transport_version != QUIC_VERSION_41
                                 ? kQuicHasMultipleAckBlocksOffset_Pre40
                                 : kQuicHasMultipleAckBlocksOffset);
  uint8_t num_ack_blocks = 0;
  uint8_t num_received_packets = 0;

  if (version_.transport_version == QUIC_VERSION_41) {
    if (has_ack_blocks && !reader->ReadUInt8(&num_ack_blocks)) {
      set_detailed_error("Unable to read num of ack blocks.");
      return false;
    }
    if (!reader->ReadUInt8(&num_received_packets)) {
      set_detailed_error("Unable to read num received packets.");
      return false;
    }
  }

  // Determine the two lengths from the frame type: largest acked length,
  // ack block length.
  const QuicPacketNumberLength ack_block_length = ReadAckPacketNumberLength(
      version_.transport_version,
      ExtractBits(frame_type, kQuicSequenceNumberLengthNumBits,
                  kActBlockLengthOffset));
  const QuicPacketNumberLength largest_acked_length = ReadAckPacketNumberLength(
      version_.transport_version,
      ExtractBits(frame_type, kQuicSequenceNumberLengthNumBits,
                  kLargestAckedOffset));

  QuicPacketNumber largest_acked;
  if (!reader->ReadBytesToUInt64(largest_acked_length, &largest_acked)) {
    set_detailed_error("Unable to read largest acked.");
    return false;
  }

  uint64_t ack_delay_time_us;
  if (!reader->ReadUFloat16(&ack_delay_time_us)) {
    set_detailed_error("Unable to read ack delay time.");
    return false;
  }

  if (!use_incremental_ack_processing_) {
    if (ack_delay_time_us == kUFloat16MaxValue) {
      ack_frame->ack_delay_time = QuicTime::Delta::Infinite();
    } else {
      ack_frame->ack_delay_time =
          QuicTime::Delta::FromMicroseconds(ack_delay_time_us);
    }
  }

  if (use_incremental_ack_processing_ &&
      !visitor_->OnAckFrameStart(
          largest_acked,
          ack_delay_time_us == kUFloat16MaxValue
              ? QuicTime::Delta::Infinite()
              : QuicTime::Delta::FromMicroseconds(ack_delay_time_us))) {
    // The visitor suppresses further processing of the packet. Although this is
    // not a parsing error, returns false as this is in middle of processing an
    // ack frame,
    set_detailed_error("Visitor suppresses further processing of ack frame.");
    return false;
  }

  if (has_ack_blocks) {
    if (version_.transport_version != QUIC_VERSION_41 &&
        !reader->ReadUInt8(&num_ack_blocks)) {
      set_detailed_error("Unable to read num of ack blocks.");
      return false;
    }
  }

  uint64_t first_block_length;
  if (!reader->ReadBytesToUInt64(ack_block_length, &first_block_length)) {
    set_detailed_error("Unable to read first ack block length.");
    return false;
  }

  if (first_block_length == 0) {
    // For non-empty ACKs, the first block length must be non-zero.
    if (largest_acked != 0 || num_ack_blocks != 0) {
      set_detailed_error(
          QuicStrCat("First block length is zero but ACK is "
                     "not empty. largest acked is ",
                     largest_acked, ", num ack blocks is ",
                     QuicTextUtils::Uint64ToString(num_ack_blocks), ".")
              .c_str());
      return false;
    }
  }

  if (first_block_length > largest_acked + 1) {
    set_detailed_error(QuicStrCat("Underflow with first ack block length ",
                                  first_block_length, " largest acked is ",
                                  largest_acked, ".")
                           .c_str());
    return false;
  }

  QuicPacketNumber first_received = largest_acked + 1 - first_block_length;
  if (use_incremental_ack_processing_) {
    if (!visitor_->OnAckRange(first_received, largest_acked + 1,
                              /*last_range=*/!has_ack_blocks)) {
      // The visitor suppresses further processing of the packet. Although
      // this is not a parsing error, returns false as this is in middle
      // of processing an ack frame,
      set_detailed_error("Visitor suppresses further processing of ack frame.");
      return false;
    }
  } else {
    ack_frame->largest_acked = largest_acked;
    ack_frame->packets.AddRange(first_received, largest_acked + 1);
  }

  if (num_ack_blocks > 0) {
    for (size_t i = 0; i < num_ack_blocks; ++i) {
      uint8_t gap = 0;
      if (!reader->ReadUInt8(&gap)) {
        set_detailed_error("Unable to read gap to next ack block.");
        return false;
      }
      uint64_t current_block_length;
      if (!reader->ReadBytesToUInt64(ack_block_length, &current_block_length)) {
        set_detailed_error("Unable to ack block length.");
        return false;
      }
      if (first_received < gap + current_block_length) {
        set_detailed_error(
            QuicStrCat("Underflow with ack block length ", current_block_length,
                       ", end of block is ", first_received - gap, ".")
                .c_str());
        return false;
      }

      first_received -= (gap + current_block_length);
      if (current_block_length > 0) {
        if (use_incremental_ack_processing_) {
          if (!visitor_->OnAckRange(first_received,
                                    first_received + current_block_length,
                                    /*last_range=*/i + 1 == num_ack_blocks)) {
            // The visitor suppresses further processing of the packet. Although
            // this is not a parsing error, returns false as this is in middle
            // of processing an ack frame,
            set_detailed_error(
                "Visitor suppresses further processing of ack frame.");
            return false;
          }
        } else {
          ack_frame->packets.AddRange(first_received,
                                      first_received + current_block_length);
        }
      }
    }
  }

  if (version_.transport_version != QUIC_VERSION_41 &&
      !reader->ReadUInt8(&num_received_packets)) {
    set_detailed_error("Unable to read num received packets.");
    return false;
  }

  if (!ProcessTimestampsInAckFrame(num_received_packets, reader, ack_frame)) {
    return false;
  }

  return true;
}

bool QuicFramer::ProcessTimestampsInAckFrame(uint8_t num_received_packets,
                                             QuicDataReader* reader,
                                             QuicAckFrame* ack_frame) {
  if (num_received_packets > 0) {
    uint8_t delta_from_largest_observed;
    if (!reader->ReadUInt8(&delta_from_largest_observed)) {
      set_detailed_error("Unable to read sequence delta in received packets.");
      return false;
    }
    QuicPacketNumber seq_num =
        LargestAcked(*ack_frame) - delta_from_largest_observed;

    // Time delta from the framer creation.
    uint32_t time_delta_us;
    if (!reader->ReadUInt32(&time_delta_us)) {
      set_detailed_error("Unable to read time delta in received packets.");
      return false;
    }

    last_timestamp_ = CalculateTimestampFromWire(time_delta_us);

    ack_frame->received_packet_times.reserve(num_received_packets);
    ack_frame->received_packet_times.emplace_back(
        seq_num, creation_time_ + last_timestamp_);

    for (uint8_t i = 1; i < num_received_packets; ++i) {
      if (!reader->ReadUInt8(&delta_from_largest_observed)) {
        set_detailed_error(
            "Unable to read sequence delta in received packets.");
        return false;
      }
      seq_num = LargestAcked(*ack_frame) - delta_from_largest_observed;

      // Time delta from the previous timestamp.
      uint64_t incremental_time_delta_us;
      if (!reader->ReadUFloat16(&incremental_time_delta_us)) {
        set_detailed_error(
            "Unable to read incremental time delta in received packets.");
        return false;
      }

      last_timestamp_ = last_timestamp_ + QuicTime::Delta::FromMicroseconds(
                                              incremental_time_delta_us);
      ack_frame->received_packet_times.emplace_back(
          seq_num, creation_time_ + last_timestamp_);
    }
  }
  return true;
}

// IETF Ack Frame consists of
//   Largest Ack'ed
//   Ack Delay
//   Ack Block Count
//   First Ack Block
//    additional gap/ack-blocks
bool QuicFramer::ProcessIetfAckFrame(QuicDataReader* reader,
                                     uint8_t frame_type,
                                     QuicAckFrame* ack_frame) {
  QuicPacketNumber largest_acked;
  if (!reader->ReadVarInt62(&largest_acked)) {
    set_detailed_error("Unable to read largest acked.");
    return false;
  }
  ack_frame->largest_acked = static_cast<QuicPacketNumber>(largest_acked);
  uint64_t ack_delay_time_in_us;
  if (!reader->ReadVarInt62(&ack_delay_time_in_us)) {
    set_detailed_error("Unable to read ack-delay-time.");
    return false;
  }

  // TODO(fkastenholz) when we get real IETF QUIC, need to get
  // the currect shift from the transport parameters.
  ack_delay_time_in_us = (ack_delay_time_in_us << kIetfAckTimestampShift);
  if (ack_delay_time_in_us == 0x3ffffffffff8) {  // May want a more
                                                 // realistic max?
    ack_frame->ack_delay_time = QuicTime::Delta::Infinite();
  } else {
    ack_frame->ack_delay_time =
        QuicTime::Delta::FromMicroseconds(ack_delay_time_in_us);
  }
  // Get number of ack blocks from the packet
  uint64_t ack_block_count;
  if (!reader->ReadVarInt62(&ack_block_count)) {
    set_detailed_error("Unable to ack block count.");
    return false;
  }

  // there always is a first ack block
  uint64_t ack_block_value;
  if (!reader->ReadVarInt62(&ack_block_value)) {
    set_detailed_error("Unable to read first ack block value.");
    return false;
  }
  // Calculate the packets being acked in the first block.
  //  +1 because AddRange is [low,high)
  QuicPacketNumber block_high = largest_acked + 1;
  QuicPacketNumber block_low = largest_acked - ack_block_value;
  ack_frame->packets.AddRange(block_low, block_high);

  while (ack_block_count != 0) {
    uint64_t gap_block_value;

    // Get the sizes of the gap and ack blocks,
    if (!reader->ReadVarInt62(&gap_block_value)) {
      set_detailed_error("Unable to read gap block value.");
      return false;
    }
    if (!reader->ReadVarInt62(&ack_block_value)) {
      set_detailed_error("Unable to read gap block value.");
      return false;
    }
    // Slide the high/low values of the ack block down to the next
    // block in the frame. +2 is because the value encoded in the
    // frame is (length_of_block- 1). Then add the range.
    block_high = block_high - (ack_block_value + gap_block_value + 2);
    block_low = block_low - (ack_block_value + gap_block_value + 2);
    ack_frame->packets.AddRange(block_low, block_high);

    // Another one done.
    ack_block_count--;
  }
  return true;
}

bool QuicFramer::ProcessStopWaitingFrame(QuicDataReader* reader,
                                         const QuicPacketHeader& header,
                                         QuicStopWaitingFrame* stop_waiting) {
  QuicPacketNumber least_unacked_delta;
  if (!reader->ReadBytesToUInt64(header.packet_number_length,
                                 &least_unacked_delta)) {
    set_detailed_error("Unable to read least unacked delta.");
    return false;
  }
  if (header.packet_number < least_unacked_delta) {
    set_detailed_error("Invalid unacked delta.");
    return false;
  }
  stop_waiting->least_unacked = header.packet_number - least_unacked_delta;

  return true;
}

bool QuicFramer::ProcessRstStreamFrame(QuicDataReader* reader,
                                       QuicRstStreamFrame* frame) {
  if (!reader->ReadUInt32(&frame->stream_id)) {
    set_detailed_error("Unable to read stream_id.");
    return false;
  }

  if (version_.transport_version != QUIC_VERSION_41) {
    if (!reader->ReadUInt64(&frame->byte_offset)) {
      set_detailed_error("Unable to read rst stream sent byte offset.");
      return false;
    }
  }

  uint32_t error_code;
  if (!reader->ReadUInt32(&error_code)) {
    set_detailed_error("Unable to read rst stream error code.");
    return false;
  }

  if (error_code >= QUIC_STREAM_LAST_ERROR) {
    // Ignore invalid stream error code if any.
    error_code = QUIC_STREAM_LAST_ERROR;
  }

  frame->error_code = static_cast<QuicRstStreamErrorCode>(error_code);

  if (version_.transport_version == QUIC_VERSION_41) {
    if (!reader->ReadUInt64(&frame->byte_offset)) {
      set_detailed_error("Unable to read rst stream sent byte offset.");
      return false;
    }
  }

  return true;
}

bool QuicFramer::ProcessConnectionCloseFrame(QuicDataReader* reader,
                                             QuicConnectionCloseFrame* frame) {
  uint32_t error_code;
  if (!reader->ReadUInt32(&error_code)) {
    set_detailed_error("Unable to read connection close error code.");
    return false;
  }

  if (error_code >= QUIC_LAST_ERROR) {
    // Ignore invalid QUIC error code if any.
    error_code = QUIC_LAST_ERROR;
  }

  frame->error_code = static_cast<QuicErrorCode>(error_code);

  QuicStringPiece error_details;
  if (!reader->ReadStringPiece16(&error_details)) {
    set_detailed_error("Unable to read connection close error details.");
    return false;
  }
  frame->error_details = error_details.as_string();

  return true;
}

bool QuicFramer::ProcessGoAwayFrame(QuicDataReader* reader,
                                    QuicGoAwayFrame* frame) {
  uint32_t error_code;
  if (!reader->ReadUInt32(&error_code)) {
    set_detailed_error("Unable to read go away error code.");
    return false;
  }

  if (error_code >= QUIC_LAST_ERROR) {
    // Ignore invalid QUIC error code if any.
    error_code = QUIC_LAST_ERROR;
  }
  frame->error_code = static_cast<QuicErrorCode>(error_code);

  uint32_t stream_id;
  if (!reader->ReadUInt32(&stream_id)) {
    set_detailed_error("Unable to read last good stream id.");
    return false;
  }
  frame->last_good_stream_id = static_cast<QuicStreamId>(stream_id);

  QuicStringPiece reason_phrase;
  if (!reader->ReadStringPiece16(&reason_phrase)) {
    set_detailed_error("Unable to read goaway reason.");
    return false;
  }
  frame->reason_phrase = reason_phrase.as_string();

  return true;
}

bool QuicFramer::ProcessWindowUpdateFrame(QuicDataReader* reader,
                                          QuicWindowUpdateFrame* frame) {
  if (!reader->ReadUInt32(&frame->stream_id)) {
    set_detailed_error("Unable to read stream_id.");
    return false;
  }

  if (!reader->ReadUInt64(&frame->byte_offset)) {
    set_detailed_error("Unable to read window byte_offset.");
    return false;
  }

  return true;
}

bool QuicFramer::ProcessBlockedFrame(QuicDataReader* reader,
                                     QuicBlockedFrame* frame) {
  if (!reader->ReadUInt32(&frame->stream_id)) {
    set_detailed_error("Unable to read stream_id.");
    return false;
  }

  return true;
}

void QuicFramer::ProcessPaddingFrame(QuicDataReader* reader,
                                     QuicPaddingFrame* frame) {
  if (version_.transport_version <= QUIC_VERSION_37) {
    frame->num_padding_bytes = reader->BytesRemaining() + 1;
    reader->ReadRemainingPayload();
    return;
  }
  // Type byte has been read.
  frame->num_padding_bytes = 1;
  uint8_t next_byte;
  while (!reader->IsDoneReading() && reader->PeekByte() == 0x00) {
    reader->ReadBytes(&next_byte, 1);
    DCHECK_EQ(0x00, next_byte);
    ++frame->num_padding_bytes;
  }
}

// static
QuicStringPiece QuicFramer::GetAssociatedDataFromEncryptedPacket(
    QuicTransportVersion version,
    const QuicEncryptedPacket& encrypted,
    QuicConnectionIdLength connection_id_length,
    bool includes_version,
    bool includes_diversification_nonce,
    QuicPacketNumberLength packet_number_length) {
  // TODO(ianswett): This is identical to QuicData::AssociatedData.
  return QuicStringPiece(
      encrypted.data(),
      GetStartOfEncryptedData(version, connection_id_length, includes_version,
                              includes_diversification_nonce,
                              packet_number_length));
}

void QuicFramer::SetDecrypter(EncryptionLevel level, QuicDecrypter* decrypter) {
  DCHECK(alternative_decrypter_ == nullptr);
  DCHECK_GE(level, decrypter_level_);
  decrypter_.reset(decrypter);
  decrypter_level_ = level;
}

void QuicFramer::SetAlternativeDecrypter(EncryptionLevel level,
                                         QuicDecrypter* decrypter,
                                         bool latch_once_used) {
  alternative_decrypter_.reset(decrypter);
  alternative_decrypter_level_ = level;
  alternative_decrypter_latch_ = latch_once_used;
}

const QuicDecrypter* QuicFramer::decrypter() const {
  return decrypter_.get();
}

const QuicDecrypter* QuicFramer::alternative_decrypter() const {
  return alternative_decrypter_.get();
}

void QuicFramer::SetEncrypter(EncryptionLevel level, QuicEncrypter* encrypter) {
  DCHECK_GE(level, 0);
  DCHECK_LT(level, NUM_ENCRYPTION_LEVELS);
  encrypter_[level].reset(encrypter);
}

size_t QuicFramer::EncryptInPlace(EncryptionLevel level,
                                  QuicPacketNumber packet_number,
                                  size_t ad_len,
                                  size_t total_len,
                                  size_t buffer_len,
                                  char* buffer) {
  size_t output_length = 0;
  if (!encrypter_[level]->EncryptPacket(
          version_.transport_version, packet_number,
          QuicStringPiece(buffer, ad_len),  // Associated data
          QuicStringPiece(buffer + ad_len, total_len - ad_len),  // Plaintext
          buffer + ad_len,  // Destination buffer
          &output_length, buffer_len - ad_len)) {
    RaiseError(QUIC_ENCRYPTION_FAILURE);
    return 0;
  }

  return ad_len + output_length;
}

size_t QuicFramer::EncryptPayload(EncryptionLevel level,
                                  QuicPacketNumber packet_number,
                                  const QuicPacket& packet,
                                  char* buffer,
                                  size_t buffer_len) {
  DCHECK(encrypter_[level] != nullptr);

  QuicStringPiece associated_data =
      packet.AssociatedData(version_.transport_version);
  // Copy in the header, because the encrypter only populates the encrypted
  // plaintext content.
  const size_t ad_len = associated_data.length();
  memmove(buffer, associated_data.data(), ad_len);
  // Encrypt the plaintext into the buffer.
  size_t output_length = 0;
  if (!encrypter_[level]->EncryptPacket(
          version_.transport_version, packet_number, associated_data,
          packet.Plaintext(version_.transport_version), buffer + ad_len,
          &output_length, buffer_len - ad_len)) {
    RaiseError(QUIC_ENCRYPTION_FAILURE);
    return 0;
  }

  return ad_len + output_length;
}

size_t QuicFramer::GetMaxPlaintextSize(size_t ciphertext_size) {
  // In order to keep the code simple, we don't have the current encryption
  // level to hand. Both the NullEncrypter and AES-GCM have a tag length of 12.
  size_t min_plaintext_size = ciphertext_size;

  for (int i = ENCRYPTION_NONE; i < NUM_ENCRYPTION_LEVELS; i++) {
    if (encrypter_[i] != nullptr) {
      size_t size = encrypter_[i]->GetMaxPlaintextSize(ciphertext_size);
      if (size < min_plaintext_size) {
        min_plaintext_size = size;
      }
    }
  }

  return min_plaintext_size;
}

bool QuicFramer::DecryptPayload(QuicDataReader* encrypted_reader,
                                const QuicPacketHeader& header,
                                const QuicEncryptedPacket& packet,
                                char* decrypted_buffer,
                                size_t buffer_length,
                                size_t* decrypted_length) {
  QuicStringPiece encrypted = encrypted_reader->ReadRemainingPayload();
  DCHECK(decrypter_ != nullptr);
  QuicStringPiece associated_data = GetAssociatedDataFromEncryptedPacket(
      version_.transport_version, packet, header.connection_id_length,
      header.version_flag, header.nonce != nullptr,
      header.packet_number_length);

  bool success = decrypter_->DecryptPacket(
      version_.transport_version, header.packet_number, associated_data,
      encrypted, decrypted_buffer, decrypted_length, buffer_length);
  if (success) {
    visitor_->OnDecryptedPacket(decrypter_level_);
  } else if (alternative_decrypter_ != nullptr) {
    if (header.nonce != nullptr) {
      DCHECK_EQ(perspective_, Perspective::IS_CLIENT);
      alternative_decrypter_->SetDiversificationNonce(*header.nonce);
    }
    bool try_alternative_decryption = true;
    if (alternative_decrypter_level_ == ENCRYPTION_INITIAL) {
      if (perspective_ == Perspective::IS_CLIENT) {
        if (header.nonce == nullptr) {
          // Can not use INITIAL decryption without a diversification nonce.
          try_alternative_decryption = false;
        }
      } else {
        DCHECK(header.nonce == nullptr);
      }
    }

    if (try_alternative_decryption) {
      success = alternative_decrypter_->DecryptPacket(
          version_.transport_version, header.packet_number, associated_data,
          encrypted, decrypted_buffer, decrypted_length, buffer_length);
    }
    if (success) {
      visitor_->OnDecryptedPacket(alternative_decrypter_level_);
      if (alternative_decrypter_latch_) {
        // Switch to the alternative decrypter and latch so that we cannot
        // switch back.
        decrypter_ = std::move(alternative_decrypter_);
        decrypter_level_ = alternative_decrypter_level_;
        alternative_decrypter_level_ = ENCRYPTION_NONE;
      } else {
        // Switch the alternative decrypter so that we use it first next time.
        decrypter_.swap(alternative_decrypter_);
        EncryptionLevel level = alternative_decrypter_level_;
        alternative_decrypter_level_ = decrypter_level_;
        decrypter_level_ = level;
      }
    }
  }

  if (!success) {
    QUIC_DVLOG(1) << ENDPOINT << "DecryptPacket failed for packet_number:"
                  << header.packet_number;
    return false;
  }

  return true;
}

size_t QuicFramer::GetAckFrameTimeStampSize(const QuicAckFrame& ack) {
  DCHECK(!use_incremental_ack_processing_);
  if (ack.received_packet_times.empty()) {
    return 0;
  }

  return 5 + 3 * (ack.received_packet_times.size() - 1);
}

size_t QuicFramer::GetAckFrameSize(
    const QuicAckFrame& ack,
    QuicPacketNumberLength packet_number_length) {
  size_t ack_size = 0;

  AckFrameInfo ack_info = GetAckFrameInfo(ack);
  QuicPacketNumberLength largest_acked_length =
      GetMinPacketNumberLength(version_.transport_version, LargestAcked(ack));
  QuicPacketNumberLength ack_block_length = GetMinPacketNumberLength(
      version_.transport_version, ack_info.max_block_length);

  ack_size =
      GetMinAckFrameSize(version_.transport_version, largest_acked_length);
  // First ack block length.
  ack_size += ack_block_length;
  if (ack_info.num_ack_blocks != 0) {
    ack_size += kNumberOfAckBlocksSize;
    ack_size += std::min(ack_info.num_ack_blocks, kMaxAckBlocks) *
                (ack_block_length + PACKET_1BYTE_PACKET_NUMBER);
  }

  // Include timestamps.
  if (!use_incremental_ack_processing_) {
    ack_size += GetAckFrameTimeStampSize(ack);
  }

  return ack_size;
}

size_t QuicFramer::ComputeFrameLength(
    const QuicFrame& frame,
    bool last_frame_in_packet,
    QuicPacketNumberLength packet_number_length) {
  switch (frame.type) {
    case STREAM_FRAME:
      return GetMinStreamFrameSize(
                 version_.transport_version, frame.stream_frame->stream_id,
                 frame.stream_frame->offset, last_frame_in_packet) +
             frame.stream_frame->data_length;
    case ACK_FRAME: {
      return GetAckFrameSize(*frame.ack_frame, packet_number_length);
    }
    case STOP_WAITING_FRAME:
      return GetStopWaitingFrameSize(version_.transport_version,
                                     packet_number_length);
    case MTU_DISCOVERY_FRAME:
      // MTU discovery frames are serialized as ping frames.
      QUIC_FALLTHROUGH_INTENDED;
    case PING_FRAME:
      // Ping has no payload.
      return kQuicFrameTypeSize;
    case RST_STREAM_FRAME:
      return GetRstStreamFrameSize();
    case CONNECTION_CLOSE_FRAME:
      return GetMinConnectionCloseFrameSize() +
             TruncateErrorString(frame.connection_close_frame->error_details)
                 .size();
    case GOAWAY_FRAME:
      return GetMinGoAwayFrameSize() +
             TruncateErrorString(frame.goaway_frame->reason_phrase).size();
    case WINDOW_UPDATE_FRAME:
      return GetWindowUpdateFrameSize();
    case BLOCKED_FRAME:
      return GetBlockedFrameSize();
    case PADDING_FRAME:
      DCHECK(false);
      return 0;
    case NUM_FRAME_TYPES:
      DCHECK(false);
      return 0;
  }

  // Not reachable, but some Chrome compilers can't figure that out.  *sigh*
  DCHECK(false);
  return 0;
}

bool QuicFramer::AppendTypeByte(const QuicFrame& frame,
                                bool no_stream_frame_length,
                                QuicDataWriter* writer) {
  uint8_t type_byte = 0;
  switch (frame.type) {
    case STREAM_FRAME: {
      if (frame.stream_frame == nullptr) {
        QUIC_BUG << "Failed to append STREAM frame with no stream_frame.";
      }
      if (version_.transport_version != QUIC_VERSION_41) {
        // Fin bit.
        type_byte |= frame.stream_frame->fin ? kQuicStreamFinMask_Pre40 : 0;

        // Data Length bit.
        type_byte <<= kQuicStreamDataLengthShift_Pre40;
        type_byte |=
            no_stream_frame_length ? 0 : kQuicStreamDataLengthMask_Pre40;

        // Offset 3 bits.
        type_byte <<= kQuicStreamShift_Pre40;
        const size_t offset_len = GetStreamOffsetSize(
            version_.transport_version, frame.stream_frame->offset);
        if (offset_len > 0) {
          type_byte |= offset_len - 1;
        }

        // stream id 2 bits.
        type_byte <<= kQuicStreamIdShift_Pre40;
        type_byte |= GetStreamIdSize(frame.stream_frame->stream_id) - 1;
        type_byte |=
            kQuicFrameTypeStreamMask_Pre40;  // Set Stream Frame Type to 1.
      } else {
        // Fin bit.
        SetBit(&type_byte, frame.stream_frame->fin, kQuicStreamFinShift);

        // Data Length bit.
        SetBit(&type_byte, !no_stream_frame_length, kQuicStreamDataLengthShift);

        // Offset 2 bits.
        uint8_t offset_len_encode = 3;
        switch (GetStreamOffsetSize(version_.transport_version,
                                    frame.stream_frame->offset)) {
          case 0:
            offset_len_encode = 0;
            break;
          case 2:
            offset_len_encode = 1;
            break;
          case 4:
            offset_len_encode = 2;
            break;
          case 8:
            offset_len_encode = 3;
            break;
          default:
            QUIC_BUG << "Invalid offset_length.";
        }
        SetBits(&type_byte, offset_len_encode, kQuicStreamOffsetNumBits,
                kQuicStreamOffsetShift);

        // stream id 2 bits.
        SetBits(&type_byte, GetStreamIdSize(frame.stream_frame->stream_id) - 1,
                kQuicStreamIDLengthNumBits, kQuicStreamIDLengthShift);
        type_byte |= kQuicFrameTypeStreamMask;  // Set Stream Frame Type to 1.
      }
      break;
    }
    case ACK_FRAME:
      return true;
    case MTU_DISCOVERY_FRAME:
      type_byte = static_cast<uint8_t>(PING_FRAME);
      break;
    default:
      type_byte = static_cast<uint8_t>(frame.type);
      break;
  }

  return writer->WriteUInt8(type_byte);
}

// static
bool QuicFramer::AppendPacketNumber(QuicPacketNumberLength packet_number_length,
                                    QuicPacketNumber packet_number,
                                    QuicDataWriter* writer) {
  size_t length = packet_number_length;
  if (length != 1 && length != 2 && length != 4 && length != 6 && length != 8) {
    QUIC_BUG << "Invalid packet_number_length: " << length;
    return false;
  }
  return writer->WriteBytesToUInt64(packet_number_length, packet_number);
}

// static
bool QuicFramer::AppendStreamId(size_t stream_id_length,
                                QuicStreamId stream_id,
                                QuicDataWriter* writer) {
  if (stream_id_length == 0 || stream_id_length > 4) {
    QUIC_BUG << "Invalid stream_id_length: " << stream_id_length;
    return false;
  }
  return writer->WriteBytesToUInt64(stream_id_length, stream_id);
}

// static
bool QuicFramer::AppendStreamOffset(size_t offset_length,
                                    QuicStreamOffset offset,
                                    QuicDataWriter* writer) {
  if (offset_length == 1 || offset_length > 8) {
    QUIC_BUG << "Invalid stream_offset_length: " << offset_length;
    return false;
  }

  return writer->WriteBytesToUInt64(offset_length, offset);
}

// static
bool QuicFramer::AppendAckBlock(uint8_t gap,
                                QuicPacketNumberLength length_length,
                                QuicPacketNumber length,
                                QuicDataWriter* writer) {
  return writer->WriteUInt8(gap) &&
         AppendPacketNumber(length_length, length, writer);
}

bool QuicFramer::AppendStreamFrame(const QuicStreamFrame& frame,
                                   bool no_stream_frame_length,
                                   QuicDataWriter* writer) {
  if (!AppendStreamId(GetStreamIdSize(frame.stream_id), frame.stream_id,
                      writer)) {
    QUIC_BUG << "Writing stream id size failed.";
    return false;
  }
  if (!AppendStreamOffset(
          GetStreamOffsetSize(version_.transport_version, frame.offset),
          frame.offset, writer)) {
    QUIC_BUG << "Writing offset size failed.";
    return false;
  }
  if (!no_stream_frame_length) {
    if ((frame.data_length > std::numeric_limits<uint16_t>::max()) ||
        !writer->WriteUInt16(static_cast<uint16_t>(frame.data_length))) {
      QUIC_BUG << "Writing stream frame length failed";
      return false;
    }
  }

  if (data_producer_ != nullptr) {
    DCHECK_EQ(nullptr, frame.data_buffer);
    if (frame.data_length == 0) {
      return true;
    }
    if (!data_producer_->WriteStreamData(frame.stream_id, frame.offset,
                                         frame.data_length, writer)) {
      QUIC_BUG << "Writing frame data failed.";
      return false;
    }
    return true;
  }

  if (!writer->WriteBytes(frame.data_buffer, frame.data_length)) {
    QUIC_BUG << "Writing frame data failed.";
    return false;
  }
  return true;
}

// Add a new ietf-format stream frame.
// Bits controlling whether there is a frame-length and frame-offset
// are in the QuicStreamFrame.
bool QuicFramer::AppendIetfStreamFrame(const QuicStreamFrame& frame,
                                       bool last_frame_in_packet,
                                       QuicDataWriter* writer) {
  // figure out whether we will have FIN, LEN, and/or OFFSET fields
  // in the packet and create a type-byte with the proper flag settings.
  uint8_t frame_type = static_cast<uint8_t>(IETF_STREAM);
  if (frame.fin) {
    frame_type |= IETF_STREAM_FRAME_FIN_BIT;
  }
  // Calculate how long the header and type byte will be. This
  // is done so that we can determine how much data can be placed in
  // the packet _after_ doing the header. Start the length with the
  // encoded size of the streamid and type byte.

  // We will include the frame offset if it is not 0
  if (frame.offset != 0) {
    frame_type |= IETF_STREAM_FRAME_OFF_BIT;
  }

  // If not the last frame in the packet, include the length field.
  if (!last_frame_in_packet) {
    frame_type |= IETF_STREAM_FRAME_LEN_BIT;
  }

  // Put the type-byte in the header.
  if (!writer->WriteUInt8(frame_type)) {
    set_detailed_error("Unable to write frame-type.");
    return false;
  }

  // Stream ID always goes in the header...
  if (!writer->WriteVarInt62(static_cast<uint64_t>(frame.stream_id))) {
    set_detailed_error("Writing stream id failed.");
    return false;
  }

  // Offset may go in the header...
  if (frame_type & IETF_STREAM_FRAME_OFF_BIT) {
    if (!writer->WriteVarInt62(static_cast<uint64_t>(frame.offset))) {
      set_detailed_error("Writing data offset failed.");
      return false;
    }
  }

  // Frame data length...
  if (frame_type & IETF_STREAM_FRAME_LEN_BIT) {
    if (!writer->WriteVarInt62(frame.data_length)) {
      set_detailed_error("Writing data length failed.");
      return false;
    }
  }

  // now move the data in
  if (data_producer_ == nullptr) {
    // data comes from the frame's buffer -- append the necessary
    // number of bytes to the frame.
    if (!writer->WriteBytes(frame.data_buffer, frame.data_length)) {
      set_detailed_error("Writing frame data failed.");
      return false;
    }
  } else {
    DCHECK_EQ(nullptr, frame.data_buffer);
    if (!data_producer_->WriteStreamData(frame.stream_id, frame.offset,
                                         frame.data_length, writer)) {
      set_detailed_error("Writing frame data failed.");
      return false;
    }
  }
  return true;
}

void QuicFramer::set_version(const ParsedQuicVersion version) {
  DCHECK(IsSupportedVersion(version)) << ParsedQuicVersionToString(version);
  version_ = version;
}

bool QuicFramer::AppendAckFrameAndTypeByte(const QuicAckFrame& frame,
                                           QuicDataWriter* writer) {
  const AckFrameInfo new_ack_info = GetAckFrameInfo(frame);
  QuicPacketNumber largest_acked = LargestAcked(frame);
  QuicPacketNumberLength largest_acked_length =
      GetMinPacketNumberLength(version_.transport_version, largest_acked);
  QuicPacketNumberLength ack_block_length = GetMinPacketNumberLength(
      version_.transport_version, new_ack_info.max_block_length);
  // Calculate available bytes for timestamps and ack blocks.
  int32_t available_timestamp_and_ack_block_bytes =
      writer->capacity() - writer->length() - ack_block_length -
      GetMinAckFrameSize(version_.transport_version, largest_acked_length) -
      (new_ack_info.num_ack_blocks != 0 ? kNumberOfAckBlocksSize : 0);
  DCHECK_LE(0, available_timestamp_and_ack_block_bytes);

  // Write out the type byte by setting the low order bits and doing shifts
  // to make room for the next bit flags to be set.
  // Whether there are multiple ack blocks.
  uint8_t type_byte = 0;
  SetBit(&type_byte, new_ack_info.num_ack_blocks != 0,
         version_.transport_version != QUIC_VERSION_41
             ? kQuicHasMultipleAckBlocksOffset_Pre40
             : kQuicHasMultipleAckBlocksOffset);

  SetBits(&type_byte, GetPacketNumberFlags(largest_acked_length),
          kQuicSequenceNumberLengthNumBits, kLargestAckedOffset);

  SetBits(&type_byte, GetPacketNumberFlags(ack_block_length),
          kQuicSequenceNumberLengthNumBits, kActBlockLengthOffset);

  if (version_.transport_version != QUIC_VERSION_41) {
    type_byte |= kQuicFrameTypeAckMask_Pre40;
  } else {
    type_byte |= kQuicFrameTypeAckMask;
  }

  if (!writer->WriteUInt8(type_byte)) {
    return false;
  }

  size_t num_timestamps_offset = 0;
  size_t max_num_ack_blocks = available_timestamp_and_ack_block_bytes /
                              (ack_block_length + PACKET_1BYTE_PACKET_NUMBER);

  // Number of ack blocks.
  size_t num_ack_blocks =
      std::min(new_ack_info.num_ack_blocks, max_num_ack_blocks);
  if (num_ack_blocks > std::numeric_limits<uint8_t>::max()) {
    num_ack_blocks = std::numeric_limits<uint8_t>::max();
  }

  if (version_.transport_version == QUIC_VERSION_41) {
    if (num_ack_blocks > 0 && !writer->WriteBytes(&num_ack_blocks, 1)) {
      return false;
    }

    // Write a placeholder for the number of timestamps which will be
    // overwritten after the ack blocks have been written.
    num_timestamps_offset = writer->length();
    uint8_t num_timestamps = 0;
    if (!writer->WriteUInt8(num_timestamps)) {
      return false;
    }
  }

  // Largest acked.
  if (!AppendPacketNumber(largest_acked_length, largest_acked, writer)) {
    return false;
  }

  // Largest acked delta time.
  uint64_t ack_delay_time_us = kUFloat16MaxValue;
  if (!frame.ack_delay_time.IsInfinite()) {
    DCHECK_LE(0u, frame.ack_delay_time.ToMicroseconds());
    ack_delay_time_us = frame.ack_delay_time.ToMicroseconds();
  }
  if (!writer->WriteUFloat16(ack_delay_time_us)) {
    return false;
  }

  if (version_.transport_version != QUIC_VERSION_41) {
    if (num_ack_blocks > 0) {
      if (!writer->WriteBytes(&num_ack_blocks, 1)) {
        return false;
      }
    }
  }

  // First ack block length.
  if (!AppendPacketNumber(ack_block_length, new_ack_info.first_block_length,
                          writer)) {
    return false;
  }

  // Ack blocks.
  if (num_ack_blocks > 0) {
    size_t num_ack_blocks_written = 0;
    // Append, in descending order from the largest ACKed packet, a series of
    // ACK blocks that represents the successfully acknoweldged packets. Each
    // appended gap/block length represents a descending delta from the previous
    // block. i.e.:
    // |--- length ---|--- gap ---|--- length ---|--- gap ---|--- largest ---|
    // For gaps larger than can be represented by a single encoded gap, a 0
    // length gap of the maximum is used, i.e.:
    // |--- length ---|--- gap ---|- 0 -|--- gap ---|--- largest ---|
    auto itr = frame.packets.rbegin();
    QuicPacketNumber previous_start = itr->min();
    ++itr;

    for (;
         itr != frame.packets.rend() && num_ack_blocks_written < num_ack_blocks;
         previous_start = itr->min(), ++itr) {
      const auto& interval = *itr;
      const QuicPacketNumber total_gap = previous_start - interval.max();
      const size_t num_encoded_gaps =
          (total_gap + std::numeric_limits<uint8_t>::max() - 1) /
          std::numeric_limits<uint8_t>::max();
      DCHECK_LE(0u, num_encoded_gaps);

      // Append empty ACK blocks because the gap is longer than a single gap.
      for (size_t i = 1;
           i < num_encoded_gaps && num_ack_blocks_written < num_ack_blocks;
           ++i) {
        if (!AppendAckBlock(std::numeric_limits<uint8_t>::max(),
                            ack_block_length, 0, writer)) {
          return false;
        }
        ++num_ack_blocks_written;
      }
      if (num_ack_blocks_written >= num_ack_blocks) {
        if (QUIC_PREDICT_FALSE(num_ack_blocks_written != num_ack_blocks)) {
          QUIC_BUG << "Wrote " << num_ack_blocks_written
                   << ", expected to write " << num_ack_blocks;
        }
        break;
      }

      const uint8_t last_gap =
          total_gap -
          (num_encoded_gaps - 1) * std::numeric_limits<uint8_t>::max();
      // Append the final ACK block with a non-empty size.
      if (!AppendAckBlock(last_gap, ack_block_length, interval.Length(),
                          writer)) {
        return false;
      }
      ++num_ack_blocks_written;
    }
    DCHECK_EQ(num_ack_blocks, num_ack_blocks_written);
  }
  // Timestamps.
  // If we don't have enough available space to append all the timestamps, don't
  // append any of them.
  if (!use_incremental_ack_processing_ &&
      writer->capacity() - writer->length() >=
          GetAckFrameTimeStampSize(frame)) {
    if (!AppendTimestampsToAckFrame(frame, num_timestamps_offset, writer)) {
      return false;
    }
  } else {
    if (transport_version() != QUIC_VERSION_41) {
      uint8_t num_received_packets = 0;
      if (!writer->WriteBytes(&num_received_packets, 1)) {
        return false;
      }
    }
  }

  return true;
}

bool QuicFramer::AppendTimestampsToAckFrame(const QuicAckFrame& frame,
                                            size_t num_timestamps_offset,
                                            QuicDataWriter* writer) {
  DCHECK_GE(std::numeric_limits<uint8_t>::max(),
            frame.received_packet_times.size());
  // num_received_packets is only 1 byte.
  if (frame.received_packet_times.size() >
      std::numeric_limits<uint8_t>::max()) {
    return false;
  }

  uint8_t num_received_packets = frame.received_packet_times.size();
  if (version_.transport_version != QUIC_VERSION_41) {
    if (!writer->WriteBytes(&num_received_packets, 1)) {
      return false;
    }
  } else {
    if (!writer->WriteUInt8AtOffset(num_received_packets,
                                    num_timestamps_offset)) {
      return false;
    }
  }
  if (num_received_packets == 0) {
    return true;
  }

  PacketTimeVector::const_iterator it = frame.received_packet_times.begin();
  QuicPacketNumber packet_number = it->first;
  QuicPacketNumber delta_from_largest_observed =
      LargestAcked(frame) - packet_number;

  DCHECK_GE(std::numeric_limits<uint8_t>::max(), delta_from_largest_observed);
  if (delta_from_largest_observed > std::numeric_limits<uint8_t>::max()) {
    return false;
  }

  if (!writer->WriteUInt8(delta_from_largest_observed)) {
    return false;
  }

  // Use the lowest 4 bytes of the time delta from the creation_time_.
  const uint64_t time_epoch_delta_us = UINT64_C(1) << 32;
  uint32_t time_delta_us =
      static_cast<uint32_t>((it->second - creation_time_).ToMicroseconds() &
                            (time_epoch_delta_us - 1));
  if (!writer->WriteUInt32(time_delta_us)) {
    return false;
  }

  QuicTime prev_time = it->second;

  for (++it; it != frame.received_packet_times.end(); ++it) {
    packet_number = it->first;
    delta_from_largest_observed = LargestAcked(frame) - packet_number;

    if (delta_from_largest_observed > std::numeric_limits<uint8_t>::max()) {
      return false;
    }

    if (!writer->WriteUInt8(delta_from_largest_observed)) {
      return false;
    }

    uint64_t frame_time_delta_us = (it->second - prev_time).ToMicroseconds();
    prev_time = it->second;
    if (!writer->WriteUFloat16(frame_time_delta_us)) {
      return false;
    }
  }
  return true;
}

bool QuicFramer::AppendStopWaitingFrame(const QuicPacketHeader& header,
                                        const QuicStopWaitingFrame& frame,
                                        QuicDataWriter* writer) {
  DCHECK_GE(header.packet_number, frame.least_unacked);
  const QuicPacketNumber least_unacked_delta =
      header.packet_number - frame.least_unacked;
  const QuicPacketNumber length_shift = header.packet_number_length * 8;

  if (least_unacked_delta >> length_shift > 0) {
    QUIC_BUG << "packet_number_length " << header.packet_number_length
             << " is too small for least_unacked_delta: " << least_unacked_delta
             << " packet_number:" << header.packet_number
             << " least_unacked:" << frame.least_unacked
             << " version:" << version_.transport_version;
    return false;
  }
  if (!AppendPacketNumber(header.packet_number_length, least_unacked_delta,
                          writer)) {
    QUIC_BUG << " seq failed: " << header.packet_number_length;
    return false;
  }

  return true;
}
// Append IETF Format Ack Frame. The IETF Ack Frame format is, basically,
//   Largest Ack'ed
//   ACK Delay
//   ACK Block Count (after the first)
//   Blocks
//    which is a series of alternating ACK and GAP blocks, each
//    containing the number of packets in the respective ACK- or
//    GAP-block, minus 1. So, for example, if the Blocks were
//    0/0/0/0/0 then it means 1-packet-acked, 1 packet in a gap,
//    1-packet-acked, 1-packet-gap, 1-packet-acked & the first
//    packet has seq# LargestAcked, the last one has seq# LargestAcked-4.

bool QuicFramer::AppendIetfAckFrameAndTypeByte(const QuicAckFrame& frame,
                                               QuicDataWriter* writer) {
  if (!writer->WriteUInt8(IETF_ACK)) {
    set_detailed_error("No room for frame-type");
    return false;
  }

  QuicPacketNumber largest_acked = LargestAcked(frame);
  if (!writer->WriteVarInt62(largest_acked)) {
    set_detailed_error("No room for largest-acked in ack frame");
    return false;
  }

  uint64_t ack_delay_time_us;
  if (frame.ack_delay_time.IsInfinite()) {
    QUIC_BUG << "Ack frame time delay is infinite";
    return false;
  }
  DCHECK_LE(0u, frame.ack_delay_time.ToMicroseconds());
  ack_delay_time_us = frame.ack_delay_time.ToMicroseconds();

  // TODO(fkastenholz) when we get real IETF QUIC, need to get
  // the currect shift from the transport parameters.
  ack_delay_time_us = ack_delay_time_us >> kIetfAckTimestampShift;
  if (!writer->WriteVarInt62(ack_delay_time_us)) {
    set_detailed_error("No room for ack-delay in ack frame");
    return false;
  }

  // Do the block-count
  uint64_t ack_block_count = frame.packets.NumIntervals();
  if (ack_block_count == 0) {
    QUIC_BUG << "Trying to build an ack frame with no ack blocks";
    return false;
  }

  // Calculate the block count we will put into the frame.
  // ID says the value is "The number of Additional ACK Block (and
  // Gap) fields after the First ACK Block." We interpret this as
  //  - n(==0) means just the First ACK Block
  //  - n(>0)  means a First Ack block followed by N pairs
  //           of Gap/Ack. So if N is 1, there is a First,
  //           a Gap, and a final Ack.
  if (!writer->WriteVarInt62(ack_block_count - 1)) {
    set_detailed_error("No room for ack block count in ack frame");
    return false;
  }
  auto itr = frame.packets.rbegin();  // first range
  // Do the first block.
  // The ranges in frame.packets are [low...high), so
  //  a) we should never see 0 and
  //  b) we need to subtract 1 when writing the value out.
  uint64_t block_length = itr->max() - itr->min();
  if (block_length == 0) {
    QUIC_BUG << "Have a 0-length range in QuicAckFrame::packets";
    return false;
  }

  if (!writer->WriteVarInt62(block_length - 1)) {
    set_detailed_error("No room for first ack block in ack frame");
    return false;
  }
  size_t previous_ack_end = itr->min();
  ack_block_count--;

  // TODO(fkastenholz) this loop adds all blocks to the frame,
  // failing if the frame buffer is not large enough. In the future,
  // we should put in as many as we can, adjusting the count to
  // indicate just what we put in. Or at least have an option to do this.
  while (ack_block_count) {
    // Do the gap separating the two ack-blocks
    // Math note: value of the gap is nr of packets separating the two
    // acks. If we have two sets of ack'd packets, 1,2,3 and 7,8,...x
    //  A) The gap size is 3 (the gap is packets 4,5,6), which is
    //     encoded in the frame as 2.
    //  B) The two frame.packets ranges are [1,4) and [7,x) so the
    //     gap calculation is (7-4)-1 ==> (3)-1 ==> 2.

    // next range
    itr++;

    // Mind the gap
    size_t gap = previous_ack_end - itr->max() - 1;

    if (!writer->WriteVarInt62(gap)) {
      set_detailed_error("No room for gap block in ack frame");
      return false;
    }

    // Add the ack-block (itr already points to it)
    block_length = itr->max() - itr->min();
    if (block_length == 0) {
      QUIC_BUG << "Have a 0-length range in QuicAckFrame::packets";
      return false;
    }

    if (!writer->WriteVarInt62(block_length - 1)) {
      set_detailed_error("No room for nth ack block in ack frame");
      return false;
    }
    previous_ack_end = itr->min();
    ack_block_count--;
  }
  return true;
}

bool QuicFramer::AppendRstStreamFrame(const QuicRstStreamFrame& frame,
                                      QuicDataWriter* writer) {
  if (!writer->WriteUInt32(frame.stream_id)) {
    return false;
  }

  if (version_.transport_version != QUIC_VERSION_41) {
    if (!writer->WriteUInt64(frame.byte_offset)) {
      return false;
    }
  }

  uint32_t error_code = static_cast<uint32_t>(frame.error_code);
  if (!writer->WriteUInt32(error_code)) {
    return false;
  }

  if (version_.transport_version == QUIC_VERSION_41) {
    if (!writer->WriteUInt64(frame.byte_offset)) {
      return false;
    }
  }

  return true;
}

bool QuicFramer::AppendConnectionCloseFrame(
    const QuicConnectionCloseFrame& frame,
    QuicDataWriter* writer) {
  uint32_t error_code = static_cast<uint32_t>(frame.error_code);
  if (!writer->WriteUInt32(error_code)) {
    return false;
  }
  if (!writer->WriteStringPiece16(TruncateErrorString(frame.error_details))) {
    return false;
  }
  return true;
}

bool QuicFramer::AppendGoAwayFrame(const QuicGoAwayFrame& frame,
                                   QuicDataWriter* writer) {
  uint32_t error_code = static_cast<uint32_t>(frame.error_code);
  if (!writer->WriteUInt32(error_code)) {
    return false;
  }
  uint32_t stream_id = static_cast<uint32_t>(frame.last_good_stream_id);
  if (!writer->WriteUInt32(stream_id)) {
    return false;
  }
  if (!writer->WriteStringPiece16(TruncateErrorString(frame.reason_phrase))) {
    return false;
  }
  return true;
}

bool QuicFramer::AppendWindowUpdateFrame(const QuicWindowUpdateFrame& frame,
                                         QuicDataWriter* writer) {
  uint32_t stream_id = static_cast<uint32_t>(frame.stream_id);
  if (!writer->WriteUInt32(stream_id)) {
    return false;
  }
  if (!writer->WriteUInt64(frame.byte_offset)) {
    return false;
  }
  return true;
}

bool QuicFramer::AppendBlockedFrame(const QuicBlockedFrame& frame,
                                    QuicDataWriter* writer) {
  uint32_t stream_id = static_cast<uint32_t>(frame.stream_id);
  if (!writer->WriteUInt32(stream_id)) {
    return false;
  }
  return true;
}

bool QuicFramer::AppendPaddingFrame(const QuicPaddingFrame& frame,
                                    QuicDataWriter* writer) {
  if (version_.transport_version <= QUIC_VERSION_37) {
    writer->WritePadding();
    return true;
  }

  if (frame.num_padding_bytes == 0) {
    return false;
  }
  if (frame.num_padding_bytes < 0) {
    QUIC_BUG_IF(frame.num_padding_bytes != -1);
    writer->WritePadding();
    return true;
  }
  // Please note, num_padding_bytes includes type byte which has been written.
  return writer->WritePaddingBytes(frame.num_padding_bytes - 1);
}

bool QuicFramer::RaiseError(QuicErrorCode error) {
  QUIC_DLOG(INFO) << ENDPOINT << "Error: " << QuicErrorCodeToString(error)
                  << " detail: " << detailed_error_;
  set_error(error);
  visitor_->OnError(this);
  return false;
}

Endianness QuicFramer::endianness() const {
  return version_.transport_version > QUIC_VERSION_38 ? NETWORK_BYTE_ORDER
                                                      : HOST_BYTE_ORDER;
}

bool QuicFramer::StartsWithChlo(QuicStreamId id,
                                QuicStreamOffset offset) const {
  if (data_producer_ == nullptr) {
    QUIC_BUG << "Does not have data producer.";
    return false;
  }
  char buf[sizeof(kCHLO)];
  QuicDataWriter writer(sizeof(kCHLO), buf, endianness());
  if (!data_producer_->WriteStreamData(id, offset, sizeof(kCHLO), &writer)) {
    QUIC_BUG << "Failed to write data for stream " << id << " with offset "
             << offset << " data_length = " << sizeof(kCHLO);
    return false;
  }

  return strncmp(buf, reinterpret_cast<const char*>(&kCHLO), sizeof(kCHLO)) ==
         0;
}

QuicStringPiece QuicFramer::TruncateErrorString(QuicStringPiece error) {
  if (error.length() <= kMaxErrorStringLength) {
    return error;
  }
  return QuicStringPiece(error.data(), kMaxErrorStringLength);
}

bool QuicFramer::AppendIetfConnectionCloseFrame(
    const QuicConnectionCloseFrame& frame,
    QuicDataWriter* writer) {
  return AppendIetfCloseFrame(IETF_CONNECTION_CLOSE,
                              static_cast<uint16_t>(frame.error_code),
                              frame.error_details, writer);
}

bool QuicFramer::AppendIetfConnectionCloseFrame(
    const QuicIetfTransportErrorCodes code,
    const QuicString& phrase,
    QuicDataWriter* writer) {
  return AppendIetfCloseFrame(
      IETF_CONNECTION_CLOSE, static_cast<const uint16_t>(code), phrase, writer);
}

bool QuicFramer::AppendIetfApplicationCloseFrame(
    const QuicConnectionCloseFrame& frame,
    QuicDataWriter* writer) {
  return AppendIetfCloseFrame(IETF_APPLICATION_CLOSE,
                              static_cast<const uint16_t>(frame.error_code),
                              frame.error_details, writer);
}
bool QuicFramer::AppendIetfApplicationCloseFrame(const uint16_t code,
                                                 const QuicString& phrase,
                                                 QuicDataWriter* writer) {
  return AppendIetfCloseFrame(IETF_APPLICATION_CLOSE, code, phrase, writer);
}
// Generate either an IETF-Connection- or IETF-Application-close frame.
// General format is
//    type-byte
//    error code (fixed 16 bits)
//    phrase length (varint)
//    phrase  (string)
bool QuicFramer::AppendIetfCloseFrame(const QuicIetfFrameType type,
                                      const uint16_t code,
                                      const QuicString& phrase,
                                      QuicDataWriter* writer) {
  if (!writer->WriteUInt8(type)) {
    set_detailed_error("Can not write close frame type byte");
    return false;
  }
  if (!writer->WriteUInt16(code)) {
    set_detailed_error("Can not write close frame code");
    return false;
  }
  if (!writer->WriteVarInt62(phrase.size())) {
    set_detailed_error("Can not write phrase-length");
    return false;
  }
  if (!phrase.empty()) {
    // append the phrase
    if (!writer->WriteBytes(static_cast<const void*>(phrase.c_str()),
                            phrase.size())) {
      set_detailed_error("Can not write phrase");
      return false;
    }
  }
  return true;
}

// Parse either an IETF-Connection- or IETF-Application-close frame.
// General format is
//    type-byte
//    error code (fixed 16 bits)
//    phrase length (varint)
//    phrase  (string)
bool QuicFramer::ProcessIetfConnectionCloseFrame(
    QuicDataReader* reader,
    const uint8_t frame_type,
    QuicConnectionCloseFrame* frame) {
  return ProcessIetfCloseFrame(reader, frame_type, frame);
}
bool QuicFramer::ProcessIetfApplicationCloseFrame(
    QuicDataReader* reader,
    const uint8_t frame_type,
    QuicConnectionCloseFrame* frame) {
  return ProcessIetfCloseFrame(reader, frame_type, frame);
}
bool QuicFramer::ProcessIetfCloseFrame(QuicDataReader* reader,
                                       const uint8_t frame_type,
                                       QuicConnectionCloseFrame* frame) {
  uint16_t code;
  if (!reader->ReadUInt16(&code)) {
    set_detailed_error("Unable to read close frame code.");
    return false;
  }
  frame->error_code = static_cast<QuicErrorCode>(code);

  uint64_t phrase_length;
  if (!reader->ReadVarInt62(&phrase_length)) {
    set_detailed_error("Unable to read phrase length");
    return false;
  }
  QuicStringPiece phrase;
  if (!reader->ReadStringPiece(&phrase, static_cast<size_t>(phrase_length))) {
    set_detailed_error("Can not read extended close information phrase");
    return false;
  }
  frame->error_details = phrase.as_string();

  return true;
}

// IETF-format Padding frames.
// Padding is just N bytes of 0x00. There is no varint62/etc
// encoding required.
bool QuicFramer::AppendIetfPaddingFrame(const QuicPaddingFrame& frame,
                                        QuicDataWriter* writer) {
  DCHECK_GT(version_.transport_version, QUIC_VERSION_37);
  // The base AppendPaddingFrame assumes that the type byte has
  // been written. It will actually write num_padding_bytes-1
  // bytes. This takes care of that issue.
  if (!writer->WriteUInt8(0)) {
    set_detailed_error("Can not write close frame type byte");
    return false;
  }
  return AppendPaddingFrame(frame, writer);
}
// Read the padding. Has to do it one byte at a time, stopping
// when we either A) reach the end of the buffer or B) reach a
// non-0x00 byte.
void QuicFramer::ProcessIetfPaddingFrame(QuicDataReader* reader,
                                         QuicPaddingFrame* frame) {
  DCHECK_GT(version_.transport_version, QUIC_VERSION_37);
  ProcessPaddingFrame(reader, frame);
}

// IETF Quic Path Challenge/Response frames.
bool QuicFramer::ProcessIetfPathChallengeFrame(QuicDataReader* reader,
                                               QuicPathChallengeFrame* frame) {
  if (!reader->ReadBytes(frame->data_buffer.data(), kQuicPathFrameBufferSize)) {
    set_detailed_error("Can not read path Challenge data");
    return false;
  }
  return true;
}
bool QuicFramer::ProcessIetfPathResponseFrame(QuicDataReader* reader,
                                              QuicPathResponseFrame* frame) {
  if (!reader->ReadBytes(frame->data_buffer.data(), kQuicPathFrameBufferSize)) {
    set_detailed_error("Can not read path Response data");
    return false;
  }
  return true;
}

bool QuicFramer::AppendIetfPathChallengeFrameAndTypeByte(
    const QuicPathChallengeFrame& frame,
    QuicDataWriter* writer) {
  if (!writer->WriteUInt8(IETF_PATH_CHALLENGE)) {
    set_detailed_error("Can not write Path Challenge frame type byte");
    return false;
  }

  if (!writer->WriteBytes(frame.data_buffer.data(), kQuicPathFrameBufferSize)) {
    set_detailed_error("Writing Path Challenge data failed.");
    return false;
  }
  return true;
}

bool QuicFramer::AppendIetfPathResponseFrameAndTypeByte(
    const QuicPathResponseFrame& frame,
    QuicDataWriter* writer) {
  if (!writer->WriteUInt8(IETF_PATH_RESPONSE)) {
    set_detailed_error("Can not write Path Response frame type byte");
    return false;
  }

  if (!writer->WriteBytes(frame.data_buffer.data(), kQuicPathFrameBufferSize)) {
    set_detailed_error("Writing Path Response data failed.");
    return false;
  }
  return true;
}

// Add a new ietf-format stream reset frame.
// General format is
//    stream id
//    application error code
//    final offset
bool QuicFramer::AppendIetfResetStreamFrame(const QuicRstStreamFrame& frame,
                                            QuicDataWriter* writer) {
  // Put the type-byte in the header.
  if (!writer->WriteUInt8(QuicIetfFrameType::IETF_RST_STREAM)) {
    set_detailed_error("Unable to write reset-stream frame-type.");
    return false;
  }
  if (!writer->WriteVarInt62(static_cast<uint64_t>(frame.stream_id))) {
    set_detailed_error("Writing reset-stream stream id failed.");
    return false;
  }
  if (!writer->WriteUInt16(static_cast<uint16_t>(frame.error_code))) {
    set_detailed_error("Writing reset-stream error code failed.");
    return false;
  }
  if (!writer->WriteVarInt62(static_cast<uint64_t>(frame.byte_offset))) {
    set_detailed_error("Writing reset-stream final-offset failed.");
    return false;
  }
  return true;
}

bool QuicFramer::ProcessIetfResetStreamFrame(QuicDataReader* reader,
                                             QuicRstStreamFrame* frame) {
  // Get Stream ID from frame. ReadVarIntStreamID returns false
  // if either A) there is a read error or B) the resulting value of
  // the Stream ID is larger than the maximum allowed value.
  if (!reader->ReadVarIntStreamId(&frame->stream_id)) {
    set_detailed_error("Reading reset-stream stream id failed.");
    return false;
  }

  uint16_t temp_uint16;
  if (!reader->ReadUInt16(&temp_uint16)) {
    set_detailed_error("Reading reset-stream error code failed.");
    return false;
  }
  frame->error_code = static_cast<QuicRstStreamErrorCode>(temp_uint16);

  if (!reader->ReadVarInt62(&frame->byte_offset)) {
    set_detailed_error("Reading reset-stream final-offset failed.");
    return false;
  }
  return true;
}

// IETF Stop Sending frames.
bool QuicFramer::ProcessIetfStopSendingFrame(
    QuicDataReader* reader,
    QuicStopSendingFrame* stop_sending_frame) {
  if (!reader->ReadVarIntStreamId(&stop_sending_frame->stream_id)) {
    set_detailed_error("Unable to read stream id");
    return false;
  }

  if (!reader->ReadUInt16(&stop_sending_frame->application_error_code)) {
    set_detailed_error("Unable to read application error code.");
    return false;
  }
  return true;
}

bool QuicFramer::AppendIetfStopSendingFrameAndTypeByte(
    const QuicStopSendingFrame& stop_sending_frame,
    QuicDataWriter* writer) {
  if (!writer->WriteUInt8(IETF_STOP_SENDING)) {
    set_detailed_error("Can not write stop sending frame type byte");
    return false;
  }
  if (!writer->WriteVarInt62(stop_sending_frame.stream_id)) {
    set_detailed_error("Can not write stop sending stream id");
    return false;
  }
  if (!writer->WriteUInt16(stop_sending_frame.application_error_code)) {
    set_detailed_error("Can not write application error code");
    return false;
  }
  return true;
}

}  // namespace net
