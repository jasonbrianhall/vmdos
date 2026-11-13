/* SPDX-License-Identifier: GPL-2.0 */
/* Hyper-V structures and constants, copied verbatim from Linux
   (include/linux/hyperv.h, drivers/scsi/storvsc_drv.c,
   drivers/net/hyperv/hyperv_net.h), so the simulated host in hvsim.cpp
   lays them out as Linux does, independently of vmdos's own offsets. */
#pragma once
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef struct { u8 b[16]; } guid_t;
#define __packed __attribute__((packed))
#define PAGE_SIZE 4096
#define MAX_USER_DEFINED_BYTES 120
#define MAX_PIPE_USER_DEFINED_BYTES 116
#define MAX_PAGE_BUFFER_COUNT 32
#define STORVSC_MAX_CMD_LEN 0x10
#define STORVSC_SENSE_BUFFER_SIZE 0x14
#define STORVSC_MAX_BUF_LEN_WITH_PADDING 0x14


struct hv_page_buffer {
	u32 len;
	u32 offset;
	u64 pfn;
};

struct hv_mpb_array {
	/* Length and Offset determines the # of pfns in the array */
	u32 len;
	u32 offset;
	u64 pfn_array[];
};

struct hv_ring_buffer {
	/* Offset in bytes from the start of ring data below */
	u32 write_index;

	/* Offset in bytes from the start of ring data below */
	u32 read_index;

	u32 interrupt_mask;

	/*
	 * WS2012/Win8 and later versions of Hyper-V implement interrupt
	 * driven flow management. The feature bit feat_pending_send_sz
	 * is set by the host on the host->guest ring buffer, and by the
	 * guest on the guest->host ring buffer.
	 *
	 * The meaning of the feature bit is a bit complex in that it has
	 * semantics that apply to both ring buffers.  If the guest sets
	 * the feature bit in the guest->host ring buffer, the guest is
	 * telling the host that:
	 * 1) It will set the pending_send_sz field in the guest->host ring
	 *    buffer when it is waiting for space to become available, and
	 * 2) It will read the pending_send_sz field in the host->guest
	 *    ring buffer and interrupt the host when it frees enough space
	 *
	 * Similarly, if the host sets the feature bit in the host->guest
	 * ring buffer, the host is telling the guest that:
	 * 1) It will set the pending_send_sz field in the host->guest ring
	 *    buffer when it is waiting for space to become available, and
	 * 2) It will read the pending_send_sz field in the guest->host
	 *    ring buffer and interrupt the guest when it frees enough space
	 *
	 * If either the guest or host does not set the feature bit that it
	 * owns, that guest or host must do polling if it encounters a full
	 * ring buffer, and not signal the other end with an interrupt.
	 */
	u32 pending_send_sz;
	u32 reserved1[12];
	union {
		struct {
			u32 feat_pending_send_sz:1;
		};
		u32 value;
	} feature_bits;

	/* Pad it to PAGE_SIZE so that data starts on page boundary */
	u8	reserved2[PAGE_SIZE - 68];

	/*
	 * Ring data starts here + RingDataStartOffset
	 * !!! DO NOT place any fields below this !!!
	 */
	u8 buffer[];
} __packed;

struct vmpacket_descriptor {
	u16 type;
	u16 offset8;
	u16 len8;
	u16 flags;
	u64 trans_id;
} __packed;

struct vmtransfer_page_range {
	u32 byte_count;
	u32 byte_offset;
} __packed;

struct vmtransfer_page_packet_header {
	struct vmpacket_descriptor d;
	u16 xfer_pageset_id;
	u8  sender_owns_set;
	u8 reserved;
	u32 range_cnt;
	struct vmtransfer_page_range ranges[];
} __packed;

struct gpa_range {
	u32 byte_count;
	u32 byte_offset;
	u64 pfn_array[];
};

enum vmbus_channel_message_type {
	CHANNELMSG_INVALID			=  0,
	CHANNELMSG_OFFERCHANNEL		=  1,
	CHANNELMSG_RESCIND_CHANNELOFFER	=  2,
	CHANNELMSG_REQUESTOFFERS		=  3,
	CHANNELMSG_ALLOFFERS_DELIVERED	=  4,
	CHANNELMSG_OPENCHANNEL		=  5,
	CHANNELMSG_OPENCHANNEL_RESULT		=  6,
	CHANNELMSG_CLOSECHANNEL		=  7,
	CHANNELMSG_GPADL_HEADER		=  8,
	CHANNELMSG_GPADL_BODY			=  9,
	CHANNELMSG_GPADL_CREATED		= 10,
	CHANNELMSG_GPADL_TEARDOWN		= 11,
	CHANNELMSG_GPADL_TORNDOWN		= 12,
	CHANNELMSG_RELID_RELEASED		= 13,
	CHANNELMSG_INITIATE_CONTACT		= 14,
	CHANNELMSG_VERSION_RESPONSE		= 15,
	CHANNELMSG_UNLOAD			= 16,
	CHANNELMSG_UNLOAD_RESPONSE		= 17,
	CHANNELMSG_18				= 18,
	CHANNELMSG_19				= 19,
	CHANNELMSG_20				= 20,
	CHANNELMSG_TL_CONNECT_REQUEST		= 21,
	CHANNELMSG_MODIFYCHANNEL		= 22,
	CHANNELMSG_TL_CONNECT_RESULT		= 23,
	CHANNELMSG_MODIFYCHANNEL_RESPONSE	= 24,
	CHANNELMSG_COUNT
};

struct vmbus_channel_message_header {
	enum vmbus_channel_message_type msgtype;
	u32 padding;
} __packed;

struct vmbus_channel_offer {
	guid_t if_type;
	guid_t if_instance;

	/*
	 * These two fields are not currently used.
	 */
	u64 reserved1;
	u64 reserved2;

	u16 chn_flags;
	u16 mmio_megabytes;		/* in bytes * 1024 * 1024 */

	union {
		/* Non-pipes: The user has MAX_USER_DEFINED_BYTES bytes. */
		struct {
			unsigned char user_def[MAX_USER_DEFINED_BYTES];
		} std;

		/*
		 * Pipes:
		 * The following structure is an integrated pipe protocol, which
		 * is implemented on top of standard user-defined data. Pipe
		 * clients have MAX_PIPE_USER_DEFINED_BYTES left for their own
		 * use.
		 */
		struct {
			u32  pipe_mode;
			unsigned char user_def[MAX_PIPE_USER_DEFINED_BYTES];
		} pipe;
	} u;
	/*
	 * The sub_channel_index is defined in Win8: a value of zero means a
	 * primary channel and a value of non-zero means a sub-channel.
	 *
	 * Before Win8, the field is reserved, meaning it's always zero.
	 */
	u16 sub_channel_index;
	u16 reserved3;
} __packed;

struct vmbus_channel_offer_channel {
	struct vmbus_channel_message_header header;
	struct vmbus_channel_offer offer;
	u32 child_relid;
	u8 monitorid;
	/*
	 * win7 and beyond splits this field into a bit field.
	 */
	u8 monitor_allocated:1;
	u8 reserved:7;
	/*
	 * These are new fields added in win7 and later.
	 * Do not access these fields without checking the
	 * negotiated protocol.
	 *
	 * If "is_dedicated_interrupt" is set, we must not set the
	 * associated bit in the channel bitmap while sending the
	 * interrupt to the host.
	 *
	 * connection_id is to be used in signaling the host.
	 */
	u16 is_dedicated_interrupt:1;
	u16 reserved1:15;
	u32 connection_id;
} __packed;

struct vmbus_channel_gpadl_header {
	struct vmbus_channel_message_header header;
	u32 child_relid;
	u32 gpadl;
	u16 range_buflen;
	u16 rangecount;
	struct gpa_range range[];
} __packed;

struct vmbus_channel_gpadl_body {
	struct vmbus_channel_message_header header;
	u32 msgnumber;
	u32 gpadl;
	u64 pfn[];
} __packed;

struct vmbus_channel_gpadl_created {
	struct vmbus_channel_message_header header;
	u32 child_relid;
	u32 gpadl;
	u32 creation_status;
} __packed;

struct vmbus_channel_open_channel {
	struct vmbus_channel_message_header header;

	/* Identifies the specific VMBus channel that is being opened. */
	u32 child_relid;

	/* ID making a particular open request at a channel offer unique. */
	u32 openid;

	/* GPADL for the channel's ring buffer. */
	u32 ringbuffer_gpadlhandle;

	/*
	 * Starting with win8, this field will be used to specify
	 * the target virtual processor on which to deliver the interrupt for
	 * the host to guest communication.
	 * Prior to win8, incoming channel interrupts would only
	 * be delivered on cpu 0. Setting this value to 0 would
	 * preserve the earlier behavior.
	 */
	u32 target_vp;

	/*
	 * The upstream ring buffer begins at offset zero in the memory
	 * described by RingBufferGpadlHandle. The downstream ring buffer
	 * follows it at this offset (in pages).
	 */
	u32 downstream_ringbuffer_pageoffset;

	/* User-specific data to be passed along to the server endpoint. */
	unsigned char userdata[MAX_USER_DEFINED_BYTES];
} __packed;

struct vmbus_channel_open_result {
	struct vmbus_channel_message_header header;
	u32 child_relid;
	u32 openid;
	u32 status;
} __packed;

struct vmbus_channel_initiate_contact {
	struct vmbus_channel_message_header header;
	u32 vmbus_version_requested;
	u32 target_vcpu; /* The VCPU the host should respond to */
	union {
		u64 interrupt_page;
		struct {
			u8	msg_sint;
			u8	msg_vtl;
			u8	reserved[2];
			u32 feature_flags; /* VMBus version 6.0 */
		};
	};
	u64 monitor_page1;
	u64 monitor_page2;
} __packed;

struct vmbus_channel_version_response {
	struct vmbus_channel_message_header header;
	u8 version_supported;

	u8 connection_state;
	u16 padding;

	/*
	 * On new hosts that support VMBus protocol 5.0, we must use
	 * VMBUS_MESSAGE_CONNECTION_ID_4 for the Initiate Contact Message,
	 * and for subsequent messages, we must use the Message Connection ID
	 * field in the host-returned Version Response Message.
	 *
	 * On old hosts, we should always use VMBUS_MESSAGE_CONNECTION_ID (1).
	 */
	u32 msg_conn_id;
} __packed;

struct vmbus_channel_packet_page_buffer {
	u16 type;
	u16 dataoffset8;
	u16 length8;
	u16 flags;
	u64 transactionid;
	u32 reserved;
	u32 rangecount;
	struct hv_page_buffer range[MAX_PAGE_BUFFER_COUNT];
} __packed;

struct vmbus_packet_mpb_array {
	u16 type;
	u16 dataoffset8;
	u16 length8;
	u16 flags;
	u64 transactionid;
	u32 reserved;
	u32 rangecount;         /* Always 1 in this case */
	struct hv_mpb_array range;
} __packed;

enum vstor_packet_operation {
	VSTOR_OPERATION_COMPLETE_IO		= 1,
	VSTOR_OPERATION_REMOVE_DEVICE		= 2,
	VSTOR_OPERATION_EXECUTE_SRB		= 3,
	VSTOR_OPERATION_RESET_LUN		= 4,
	VSTOR_OPERATION_RESET_ADAPTER		= 5,
	VSTOR_OPERATION_RESET_BUS		= 6,
	VSTOR_OPERATION_BEGIN_INITIALIZATION	= 7,
	VSTOR_OPERATION_END_INITIALIZATION	= 8,
	VSTOR_OPERATION_QUERY_PROTOCOL_VERSION	= 9,
	VSTOR_OPERATION_QUERY_PROPERTIES	= 10,
	VSTOR_OPERATION_ENUMERATE_BUS		= 11,
	VSTOR_OPERATION_FCHBA_DATA              = 12,
	VSTOR_OPERATION_CREATE_SUB_CHANNELS     = 13,
	VSTOR_OPERATION_MAXIMUM                 = 13
};

struct vmscsi_request {
	u16 length;
	u8 srb_status;
	u8 scsi_status;

	u8  port_number;
	u8  path_id;
	u8  target_id;
	u8  lun;

	u8  cdb_length;
	u8  sense_info_length;
	u8  data_in;
	u8  reserved;

	u32 data_transfer_length;

	union {
		u8 cdb[STORVSC_MAX_CMD_LEN];
		u8 sense_data[STORVSC_SENSE_BUFFER_SIZE];
		u8 reserved_array[STORVSC_MAX_BUF_LEN_WITH_PADDING];
	};
	/*
	 * The following was added in win8.
	 */
	u16 reserve;
	u8  queue_tag;
	u8  queue_action;
	u32 srb_flags;
	u32 time_out_value;
	u32 queue_sort_ey;

} __attribute((packed));

struct vmstorage_channel_properties {
	u32 reserved;
	u16 max_channel_cnt;
	u16 reserved1;

	u32 flags;
	u32   max_transfer_bytes;

	u64  reserved2;
} __packed;

struct vmstorage_protocol_version {
	/* Major (MSW) and minor (LSW) version numbers. */
	u16 major_minor;

	/*
	 * Revision number is auto-incremented whenever this file is changed
	 * (See FILL_VMSTOR_REVISION macro above).  Mismatch does not
	 * definitely indicate incompatibility--but it does indicate mismatched
	 * builds.
	 * This is only used on the windows side. Just set it to 0.
	 */
	u16 revision;
} __packed;

struct hv_fc_wwn_packet {
	u8	primary_active;
	u8	reserved1[3];
	u8	primary_port_wwn[8];
	u8	primary_node_wwn[8];
	u8	secondary_port_wwn[8];
	u8	secondary_node_wwn[8];
};

struct vstor_packet {
	/* Requested operation type */
	enum vstor_packet_operation operation;

	/*  Flags - see below for values */
	u32 flags;

	/* Status of the request returned from the server side. */
	u32 status;

	/* Data payload area */
	union {
		/*
		 * Structure used to forward SCSI commands from the
		 * client to the server.
		 */
		struct vmscsi_request vm_srb;

		/* Structure used to query channel properties. */
		struct vmstorage_channel_properties storage_channel_properties;

		/* Used during version negotiations. */
		struct vmstorage_protocol_version version;

		/* Fibre channel address packet */
		struct hv_fc_wwn_packet wwn_packet;

		/* Number of sub-channels to create */
		u16 sub_channel_count;

		/* This will be the maximum of the union members */
		u8  buffer[0x34];
	};
} __packed;

enum {
	NVSP_MSG_TYPE_NONE = 0,

	/* Init Messages */
	NVSP_MSG_TYPE_INIT			= 1,
	NVSP_MSG_TYPE_INIT_COMPLETE		= 2,

	NVSP_VERSION_MSG_START			= 100,

	/* Version 1 Messages */
	NVSP_MSG1_TYPE_SEND_NDIS_VER		= NVSP_VERSION_MSG_START,

	NVSP_MSG1_TYPE_SEND_RECV_BUF,
	NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE,
	NVSP_MSG1_TYPE_REVOKE_RECV_BUF,

	NVSP_MSG1_TYPE_SEND_SEND_BUF,
	NVSP_MSG1_TYPE_SEND_SEND_BUF_COMPLETE,
	NVSP_MSG1_TYPE_REVOKE_SEND_BUF,

	NVSP_MSG1_TYPE_SEND_RNDIS_PKT,
	NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE,

	/* Version 2 messages */
	NVSP_MSG2_TYPE_SEND_CHIMNEY_DELEGATED_BUF,
	NVSP_MSG2_TYPE_SEND_CHIMNEY_DELEGATED_BUF_COMP,
	NVSP_MSG2_TYPE_REVOKE_CHIMNEY_DELEGATED_BUF,

	NVSP_MSG2_TYPE_RESUME_CHIMNEY_RX_INDICATION,

	NVSP_MSG2_TYPE_TERMINATE_CHIMNEY,
	NVSP_MSG2_TYPE_TERMINATE_CHIMNEY_COMP,

	NVSP_MSG2_TYPE_INDICATE_CHIMNEY_EVENT,

	NVSP_MSG2_TYPE_SEND_CHIMNEY_PKT,
	NVSP_MSG2_TYPE_SEND_CHIMNEY_PKT_COMP,

	NVSP_MSG2_TYPE_POST_CHIMNEY_RECV_REQ,
	NVSP_MSG2_TYPE_POST_CHIMNEY_RECV_REQ_COMP,

	NVSP_MSG2_TYPE_ALLOC_RXBUF,
	NVSP_MSG2_TYPE_ALLOC_RXBUF_COMP,

	NVSP_MSG2_TYPE_FREE_RXBUF,

	NVSP_MSG2_TYPE_SEND_VMQ_RNDIS_PKT,
	NVSP_MSG2_TYPE_SEND_VMQ_RNDIS_PKT_COMP,

	NVSP_MSG2_TYPE_SEND_NDIS_CONFIG,

	NVSP_MSG2_TYPE_ALLOC_CHIMNEY_HANDLE,
	NVSP_MSG2_TYPE_ALLOC_CHIMNEY_HANDLE_COMP,

	NVSP_MSG2_MAX = NVSP_MSG2_TYPE_ALLOC_CHIMNEY_HANDLE_COMP,

	/* Version 4 messages */
	NVSP_MSG4_TYPE_SEND_VF_ASSOCIATION,
	NVSP_MSG4_TYPE_SWITCH_DATA_PATH,
	NVSP_MSG4_TYPE_UPLINK_CONNECT_STATE_DEPRECATED,

	NVSP_MSG4_MAX = NVSP_MSG4_TYPE_UPLINK_CONNECT_STATE_DEPRECATED,

	/* Version 5 messages */
	NVSP_MSG5_TYPE_OID_QUERY_EX,
	NVSP_MSG5_TYPE_OID_QUERY_EX_COMP,
	NVSP_MSG5_TYPE_SUBCHANNEL,
	NVSP_MSG5_TYPE_SEND_INDIRECTION_TABLE,

	NVSP_MSG5_MAX = NVSP_MSG5_TYPE_SEND_INDIRECTION_TABLE,

	/* Version 6 messages */
	NVSP_MSG6_TYPE_PD_API,
	NVSP_MSG6_TYPE_PD_POST_BATCH,

	NVSP_MSG6_MAX = NVSP_MSG6_TYPE_PD_POST_BATCH
};

enum {
	NVSP_STAT_NONE = 0,
	NVSP_STAT_SUCCESS,
	NVSP_STAT_FAIL,
	NVSP_STAT_PROTOCOL_TOO_NEW,
	NVSP_STAT_PROTOCOL_TOO_OLD,
	NVSP_STAT_INVALID_RNDIS_PKT,
	NVSP_STAT_BUSY,
	NVSP_STAT_PROTOCOL_UNSUPPORTED,
	NVSP_STAT_MAX,
};

struct nvsp_message_header {
	u32 msg_type;
};

struct nvsp_message_init {
	u32 min_protocol_ver;
	u32 max_protocol_ver;
} __packed;

struct nvsp_message_init_complete {
	u32 negotiated_protocol_ver;
	u32 max_mdl_chain_len;
	u32 status;
} __packed;

union nvsp_message_init_uber {
	struct nvsp_message_init init;
	struct nvsp_message_init_complete init_complete;
} __packed;

struct nvsp_1_message_send_ndis_version {
	u32 ndis_major_ver;
	u32 ndis_minor_ver;
} __packed;

struct nvsp_1_message_send_receive_buffer {
	u32 gpadl_handle;
	u16 id;
} __packed;

struct nvsp_1_receive_buffer_section {
	u32 offset;
	u32 sub_alloc_size;
	u32 num_sub_allocs;
	u32 end_offset;
} __packed;

struct nvsp_1_message_send_receive_buffer_complete {
	u32 status;
	u32 num_sections;

	/*
	 * The receive buffer is split into two parts, a large suballocation
	 * section and a small suballocation section. These sections are then
	 * suballocated by a certain size.
	 */

	/*
	 * For example, the following break up of the receive buffer has 6
	 * large suballocations and 10 small suballocations.
	 */

	/*
	 * |            Large Section          |  |   Small Section   |
	 * ------------------------------------------------------------
	 * |     |     |     |     |     |     |  | | | | | | | | | | |
	 * |                                      |
	 *  LargeOffset                            SmallOffset
	 */

	struct nvsp_1_receive_buffer_section sections[];
} __packed;

struct nvsp_1_message_revoke_receive_buffer {
	u16 id;
};

struct nvsp_1_message_send_send_buffer {
	u32 gpadl_handle;
	u16 id;
} __packed;

struct nvsp_1_message_send_send_buffer_complete {
	u32 status;

	/*
	 * The VSC gets to choose the size of the send buffer and the VSP gets
	 * to choose the sections size of the buffer.  This was done to enable
	 * dynamic reconfigurations when the cost of GPA-direct buffers
	 * decreases.
	 */
	u32 section_size;
} __packed;

struct nvsp_1_message_revoke_send_buffer {
	u16 id;
};

struct nvsp_1_message_send_rndis_packet {
	/*
	 * This field is specified by RNDIS. They assume there's two different
	 * channels of communication. However, the Network VSP only has one.
	 * Therefore, the channel travels with the RNDIS packet.
	 */
	u32 channel_type;

	/*
	 * This field is used to send part or all of the data through a send
	 * buffer. This values specifies an index into the send buffer. If the
	 * index is 0xFFFFFFFF, then the send buffer is not being used and all
	 * of the data was sent through other VMBus mechanisms.
	 */
	u32 send_buf_section_index;
	u32 send_buf_section_size;
} __packed;

struct nvsp_1_message_send_rndis_packet_complete {
	u32 status;
};

union nvsp_1_message_uber {
	struct nvsp_1_message_send_ndis_version send_ndis_ver;

	struct nvsp_1_message_send_receive_buffer send_recv_buf;
	struct nvsp_1_message_send_receive_buffer_complete
						send_recv_buf_complete;
	struct nvsp_1_message_revoke_receive_buffer revoke_recv_buf;

	struct nvsp_1_message_send_send_buffer send_send_buf;
	struct nvsp_1_message_send_send_buffer_complete send_send_buf_complete;
	struct nvsp_1_message_revoke_send_buffer revoke_send_buf;

	struct nvsp_1_message_send_rndis_packet send_rndis_pkt;
	struct nvsp_1_message_send_rndis_packet_complete
						send_rndis_pkt_complete;
} __packed;

struct nvsp_2_vsc_capability {
	union {
		u64 data;
		struct {
			u64 vmq:1;
			u64 chimney:1;
			u64 sriov:1;
			u64 ieee8021q:1;
			u64 correlation_id:1;
			u64 teaming:1;
			u64 vsubnetid:1;
			u64 rsc:1;
		};
	};
} __packed;

struct nvsp_2_send_ndis_config {
	u32 mtu;
	u32 reserved;
	struct nvsp_2_vsc_capability capability;
} __packed;

struct nvsp_2_alloc_rxbuf {
	/* Allocation ID to match the allocation request and response */
	u32 alloc_id;

	/* Length of the VM shared memory receive buffer that needs to
	 * be allocated
	 */
	u32 len;
} __packed;

struct nvsp_2_alloc_rxbuf_comp {
	/* The NDIS_STATUS code for buffer allocation */
	u32 status;

	u32 alloc_id;

	/* GPADL handle for the allocated receive buffer */
	u32 gpadl_handle;

	/* Receive buffer ID */
	u64 recv_buf_id;
} __packed;

struct nvsp_2_free_rxbuf {
	u64 recv_buf_id;
} __packed;

union nvsp_2_message_uber {
	struct nvsp_2_send_ndis_config send_ndis_config;
	struct nvsp_2_alloc_rxbuf alloc_rxbuf;
	struct nvsp_2_alloc_rxbuf_comp alloc_rxbuf_comp;
	struct nvsp_2_free_rxbuf free_rxbuf;
} __packed;

struct nvsp_4_send_vf_association {
	/* 1: allocated, serial number is valid. 0: not allocated */
	u32 allocated;

	/* Serial number of the VF to team with */
	u32 serial;
} __packed;

struct nvsp_4_sw_datapath {
	u32 active_datapath; /* active data path in VM */
} __packed;

union nvsp_4_message_uber {
	struct nvsp_4_send_vf_association vf_assoc;
	struct nvsp_4_sw_datapath active_dp;
} __packed;

struct nvsp_5_subchannel_request {
	u32 op;
	u32 num_subchannels;
} __packed;

struct nvsp_5_subchannel_complete {
	u32 status;
	u32 num_subchannels; /* Actual number of subchannels allocated */
} __packed;

struct nvsp_5_send_indirect_table {
	/* The number of entries in the send indirection table */
	u32 count;

	/* The offset of the send indirection table from the beginning of
	 * struct nvsp_message.
	 * The send indirection table tells which channel to put the send
	 * traffic on. Each entry is a channel number.
	 */
	u32 offset;
} __packed;

union nvsp_5_message_uber {
	struct nvsp_5_subchannel_request subchn_req;
	struct nvsp_5_subchannel_complete subchn_comp;
	struct nvsp_5_send_indirect_table send_table;
} __packed;

struct grp_affinity {
	u64 mask;
	u16 grp;
	u16 reserved[3];
} __packed;

struct nvsp_6_pd_api_req {
	u32 op;

	union {
		/* MMIO information is sent from the VM to VSP */
		struct __packed {
			u64 mmio_pa; /* MMIO Physical Address */
			u32 mmio_len;

			/* Number of PD queues a VM can support */
			u16 num_subchn;
		} config;

		/* Switch Datapath */
		struct __packed {
			/* Host Datapath Is PacketDirect */
			u8 host_dpath_is_pd;

			/* Guest PacketDirect Is Enabled */
			u8 guest_pd_enabled;
		} sw_dpath;

		/* Open Provider*/
		struct __packed {
			u32 prov_id; /* Provider id */
			u32 flag;
		} open_prov;

		/* Close Provider */
		struct __packed {
			u32 prov_id;
		} cls_prov;

		/* Create Queue*/
		struct __packed {
			u32 prov_id;
			u16 q_id;
			u16 q_size;
			u8 is_recv_q;
			u8 is_rss_q;
			u32 recv_data_len;
			struct grp_affinity affy;
		} cr_q;

		/* Delete Queue*/
		struct __packed {
			u32 prov_id;
			u16 q_id;
		} del_q;

		/* Flush Queue */
		struct __packed {
			u32 prov_id;
			u16 q_id;
		} flush_q;

		/* Allocate Common Buffer */
		struct __packed {
			u32 len;
			u32 pf_node; /* Preferred Node */
			u16 region_id;
		} alloc_com_buf;

		/* Free Common Buffer */
		struct __packed {
			u32 len;
			u64 pa; /* Physical Address */
			u32 pf_node; /* Preferred Node */
			u16 region_id;
			u8 cache_type;
		} free_com_buf;
	} __packed;
} __packed;

struct nvsp_6_pd_api_comp {
	u32 op;
	u32 status;

	union {
		struct __packed {
			/* actual number of PD queues allocated to the VM */
			u16 num_pd_q;

			/* Num Receive Rss PD Queues */
			u8 num_rss_q;

			u8 is_supported; /* Is supported by VSP */
			u8 is_enabled; /* Is enabled by VSP */
		} config;

		/* Open Provider */
		struct __packed {
			u32 prov_id;
		} open_prov;

		/* Create Queue */
		struct __packed {
			u32 prov_id;
			u16 q_id;
			u16 q_size;
			u32 recv_data_len;
			struct grp_affinity affy;
		} cr_q;

		/* Allocate Common Buffer */
		struct __packed {
			u64 pa; /* Physical Address */
			u32 len;
			u32 pf_node; /* Preferred Node */
			u16 region_id;
			u8 cache_type;
		} alloc_com_buf;
	} __packed;
} __packed;

union nvsp_6_message_uber {
	struct nvsp_6_pd_api_req pd_req;
	struct nvsp_6_pd_api_comp pd_comp;
} __packed;

union nvsp_all_messages {
	union nvsp_message_init_uber init_msg;
	union nvsp_1_message_uber v1_msg;
	union nvsp_2_message_uber v2_msg;
	union nvsp_4_message_uber v4_msg;
	union nvsp_5_message_uber v5_msg;
	union nvsp_6_message_uber v6_msg;
} __packed;

struct nvsp_message {
	struct nvsp_message_header hdr;
	union nvsp_all_messages msg;
} __packed;

struct rndis_initialize_request {
	u32 req_id;
	u32 major_ver;
	u32 minor_ver;
	u32 max_xfer_size;
};

struct rndis_initialize_complete {
	u32 req_id;
	u32 status;
	u32 major_ver;
	u32 minor_ver;
	u32 dev_flags;
	u32 medium;
	u32 max_pkt_per_msg;
	u32 max_xfer_size;
	u32 pkt_alignment_factor;
	u32 af_list_offset;
	u32 af_list_size;
};

struct rndis_query_request {
	u32 req_id;
	u32 oid;
	u32 info_buflen;
	u32 info_buf_offset;
	u32 dev_vc_handle;
};

struct rndis_query_complete {
	u32 req_id;
	u32 status;
	u32 info_buflen;
	u32 info_buf_offset;
};

struct rndis_set_request {
	u32 req_id;
	u32 oid;
	u32 info_buflen;
	u32 info_buf_offset;
	u32 dev_vc_handle;
	u8  info_buf[];
};

struct rndis_set_complete {
	u32 req_id;
	u32 status;
};

struct rndis_indicate_status {
	u32 status;
	u32 status_buflen;
	u32 status_buf_offset;
};

struct rndis_packet {
	u32 data_offset;
	u32 data_len;
	u32 oob_data_offset;
	u32 oob_data_len;
	u32 num_oob_data_elements;
	u32 per_pkt_info_offset;
	u32 per_pkt_info_len;
	u32 vc_handle;
	u32 reserved;
};
