/*
 * Deluge USB Controller - Descriptor Definitions
 */

#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

// Unit numbers for UAC2 entities - Speaker only
#define UAC2_ENTITY_CLOCK 0x04
#define UAC2_ENTITY_SPK_INPUT_TERMINAL 0x01
#define UAC2_ENTITY_SPK_FEATURE_UNIT 0x02
#define UAC2_ENTITY_SPK_OUTPUT_TERMINAL 0x03

// Interface numbers (must match descriptor order)
// Audio first for Windows UAC2 driver compatibility
enum {
	ITF_NUM_AUDIO_CONTROL = 0,
	ITF_NUM_AUDIO_STREAMING_SPK,
	ITF_NUM_CDC,
	ITF_NUM_CDC_DATA,
	ITF_NUM_MIDI,
	ITF_NUM_MIDI_STREAMING,
	ITF_NUM_TOTAL
};

// Speaker-only descriptor length - dual format (16-bit and 24-bit)
#define TUD_AUDIO_SPEAKER_STEREO_DESC_LEN                                                                              \
	(TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN                               \
	 + TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2)           \
	 + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN + TUD_AUDIO20_DESC_STD_AC_INT_EP_LEN /* Interface 1, Alternate 0 (idle) */     \
	 + TUD_AUDIO20_DESC_STD_AS_LEN                                           /* Interface 1, Alternate 1 (16-bit) */   \
	 + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN               \
	 + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN /* Interface 1, Alternate 2 (24-bit) */  \
	 + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN               \
	 + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

// Speaker-only descriptor macro - stereo output, 24-bit 44.1kHz only
#define TUD_AUDIO_SPEAKER_STEREO_DESCRIPTOR(_stridx, _epout, _epint)                                                   \
	/* Standard Interface Association Descriptor (IAD) */                                                              \
	TUD_AUDIO20_DESC_IAD(/*_firstitf*/ ITF_NUM_AUDIO_CONTROL, /*_nitfs*/ 2,                                            \
	                     /*_stridx*/ 0x00), /* Standard AC Interface Descriptor(4.7.1) */                              \
	    TUD_AUDIO20_DESC_STD_AC(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_nEPs*/ 0x01,                                     \
	                            /*_stridx*/ _stridx), /* Class-Specific AC Interface Header Descriptor(4.7.2) */       \
	    TUD_AUDIO20_DESC_CS_AC(                                                                                        \
	        /*_bcdADC*/ 0x0200, /*_category*/ AUDIO20_FUNC_IO_BOX, /*_totallen*/                                       \
	        TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO20_DESC_INPUT_TERM_LEN      \
	            + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN,                                                                    \
	        /*_ctrl*/ AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS), /* Clock Source Descriptor(4.7.2.1) */                \
	    TUD_AUDIO20_DESC_CLK_SRC(                                                                                      \
	        /*_clkid*/ UAC2_ENTITY_CLOCK, /*_attr*/ 3, /*_ctrl*/ 7, /*_assocTerm*/ 0x00,                               \
	        /*_stridx*/ 0x00), /* Input Terminal Descriptor(4.7.2.4) - USB to Deluge (speaker output) */               \
	    TUD_AUDIO20_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,                                        \
	                                /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00,                  \
	                                /*_clkid*/ UAC2_ENTITY_CLOCK, /*_nchannelslogical*/ 0x02,                          \
	                                /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00,  \
	                                /*_ctrl*/ 0 * (AUDIO20_CTRL_R << AUDIO20_IN_TERM_CTRL_CONNECTOR_POS),              \
	                                /*_stridx*/ 0x00), /* Feature Unit Descriptor(4.7.2.8) */                          \
	    TUD_AUDIO20_DESC_FEATURE_UNIT(                                                                                 \
	        /*_unitid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, /*_srcid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL,                       \
	        /*_stridx*/ 0x00, /*_ctrlch0master*/                                                                       \
	        (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS                                                     \
	         | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), /*_ctrlch1*/                                  \
	        (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS                                                     \
	         | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS), /*_ctrlch2*/                                  \
	        (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS                                                     \
	         | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)), /* Output Terminal Descriptor(4.7.2.5) */    \
	    TUD_AUDIO20_DESC_OUTPUT_TERM(                                                                                  \
	        /*_termid*/ UAC2_ENTITY_SPK_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES,                 \
	        /*_assocTerm*/ 0x00, /*_srcid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, /*_clkid*/ UAC2_ENTITY_CLOCK,                \
	        /*_ctrl*/ 0x0000, /*_stridx*/ 0x00), /* Standard AC Interrupt Endpoint Descriptor(4.8.2.1) */              \
	    TUD_AUDIO20_DESC_STD_AC_INT_EP(/*_ep*/ _epint, /*_interval*/ 0x01),                                            \
	    /* ===== Interface 1: Speaker Streaming ===== */ /* Standard AS Interface                                      \
	                                                        Descriptor(4.9.1) - Interface 1,                           \
	                                                        Alternate 0 (idle) */                                      \
	    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_SPK), /*_altset*/ 0x00,              \
	                                /*_nEPs*/ 0x00, /*_stridx*/ 0x05), /* Standard AS Interface Descriptor(4.9.1) -    \
	                                                                      Interface 1, Alternate 1 (24-bit format) */  \
	    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_SPK), /*_altset*/ 0x01,              \
	                                /*_nEPs*/ 0x01,                                                                    \
	                                /*_stridx*/ 0x05), /* Class-Specific AS Interface Descriptor(4.9.2) */             \
	    TUD_AUDIO20_DESC_CS_AS_INT(/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_ctrl*/ AUDIO20_CTRL_NONE,            \
	                               /*_formattype*/ AUDIO20_FORMAT_TYPE_I, /*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM, \
	                               /*_nchannelsphysical*/ CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX,                          \
	                               /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,                              \
	                               /*_stridx*/ 0x00), /* Type I Format Type Descriptor(2.3.1.6) */                     \
	    TUD_AUDIO20_DESC_TYPE_I_FORMAT(                                                                                \
	        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,                                                       \
	        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX), /* Standard AS Isochronous Audio Data Endpoint               \
	                                                         Descriptor(4.10.1.1) - 16-bit */                          \
	    TUD_AUDIO20_DESC_STD_AS_ISO_EP(                                                                                \
	        /*_ep*/ _epout, /*_attr*/                                                                                  \
	        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE                               \
	                  | (uint8_t)TUSB_ISO_EP_ATT_DATA), /*_maxEPsize*/                                                 \
	        CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT,                                                                            \
	        /*_interval*/ 0x01), /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */          \
	    TUD_AUDIO20_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,                      \
	                                  /*_ctrl*/ AUDIO20_CTRL_NONE,                                                     \
	                                  /*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,           \
	                                  /*_lockdelay*/ 0x0001), /* Standard AS Interface Descriptor(4.9.1) - Interface   \
	                                                  1, Alternate 2 (24-bit format) */                                \
	    TUD_AUDIO20_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)(ITF_NUM_AUDIO_STREAMING_SPK), /*_altset*/ 0x02,              \
	                                /*_nEPs*/ 0x01,                                                                    \
	                                /*_stridx*/ 0x05), /* Class-Specific AS Interface Descriptor(4.9.2) */             \
	    TUD_AUDIO20_DESC_CS_AS_INT(                                                                                    \
	        /*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, /*_ctrl*/ AUDIO20_CTRL_NONE,                                   \
	        /*_formattype*/ AUDIO20_FORMAT_TYPE_I, /*_formats*/ AUDIO20_DATA_FORMAT_TYPE_I_PCM,                        \
	        /*_nchannelsphysical*/ CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX,                                                 \
	        /*_channelcfg*/ AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED,                                                     \
	        /*_stridx*/ 0x00), /* Type I Format Type Descriptor(2.3.1.6) - 24-bit in 32-bit slots */                   \
	    TUD_AUDIO20_DESC_TYPE_I_FORMAT(                                                                                \
	        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_N_BYTES_PER_SAMPLE_RX,                                                       \
	        CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX), /* Standard AS Isochronous Audio Data Endpoint               \
	                                                         Descriptor(4.10.1.1) - 24-bit */                          \
	    TUD_AUDIO20_DESC_STD_AS_ISO_EP(                                                                                \
	        /*_ep*/ _epout, /*_attr*/                                                                                  \
	        (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE                               \
	                  | (uint8_t)TUSB_ISO_EP_ATT_DATA), /*_maxEPsize*/                                                 \
	        CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT,                                                                            \
	        /*_interval*/ 0x01), /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.10.1.2) */          \
	    TUD_AUDIO20_DESC_CS_AS_ISO_EP(                                                                                 \
	        /*_attr*/ AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, /*_ctrl*/ AUDIO20_CTRL_NONE,                   \
	        /*_lockdelayunit*/ AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001)

#endif // USB_DESCRIPTORS_H_
