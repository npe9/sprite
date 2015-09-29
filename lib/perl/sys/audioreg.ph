sub AUDIO_PACK {
    local($reg,$length) = @_;
    eval "((($reg) << 8) | ($length))";
}
sub AUDIO_UNPACK_REG {
    local($x) = @_;
    eval "((($x) >> 8) & 0xff)";
}
sub AUDIO_UNPACK_LENGTH {
    local($x) = @_;
    eval "(($x) & 0xff)";
}
sub AUDIO_INIT {0x20;}
sub AUDIO_INIT_INIT { &AUDIO_PACK(( &AUDIO_INIT | 0x01),1);}
sub AUDIO_MUX {0x40;}
sub AUDIO_MUX_MCR1 { &AUDIO_PACK( &AUDIO_MUX | 0x01,1);}
sub AUDIO_MUX_MCR2 { &AUDIO_PACK( &AUDIO_MUX | 0x02,1);}
sub AUDIO_MUX_MCR3 { &AUDIO_PACK( &AUDIO_MUX | 0x03,1);}
sub AUDIO_MUX_MCR4 { &AUDIO_PACK( &AUDIO_MUX | 0x04,1);}
sub AUDIO_MAP {0x60;}
sub AUDIO_MAP_X { &AUDIO_PACK( &AUDIO_MAP | 0x01,16);}
sub AUDIO_MAP_R { &AUDIO_PACK( &AUDIO_MAP | 0x02,16);}
sub AUDIO_MAP_GX { &AUDIO_PACK( &AUDIO_MAP | 0x03,2);}
sub AUDIO_MAP_GR { &AUDIO_PACK( &AUDIO_MAP | 0x04,2);}
sub AUDIO_MAP_GER { &AUDIO_PACK( &AUDIO_MAP | 0x05,2);}
sub AUDIO_MAP_STG { &AUDIO_PACK( &AUDIO_MAP | 0x06,2);}
sub AUDIO_MAP_FTGR { &AUDIO_PACK( &AUDIO_MAP | 0x07,2);}
sub AUDIO_MAP_ATGR { &AUDIO_PACK( &AUDIO_MAP | 0x08,2);}
sub AUDIO_MAP_MMR1 { &AUDIO_PACK( &AUDIO_MAP | 0x09,1);}
sub AUDIO_MAP_MMR2 { &AUDIO_PACK( &AUDIO_MAP | 0x0a,1);}
sub AUDIO_MAP_ALL { &AUDIO_PACK( &AUDIO_MAP | 0x0b,46);}
sub AUDIO_MAP_GX_MIN {0;}
sub AUDIO_MAP_GX_MAX {12;}
sub AUDIO_MAP_GR_MIN {-12;}
sub AUDIO_MAP_GR_MAX {0;}
sub AUDIO_MAP_GER_MIN {-10;}
sub AUDIO_MAP_GER_MAX {18;}
sub AUDIO_MAP_STG_MIN {-18;}
sub AUDIO_MAP_STG_MAX {0;}
sub AUDIO_MAP_FTGR_MIN {16;}
sub AUDIO_MAP_FTGR_MAX {3999;}
sub AUDIO_MAP_ATGR_MIN {-18;}
sub AUDIO_MAP_ATGR_MAX {0;}
sub AUDIO_INIT_BITS_IDLE {0x00;}
sub AUDIO_INIT_BITS_ACTIVE {0x01;}
sub AUDIO_INIT_BITS_NOMAP {0x20;}
sub AUDIO_INIT_BITS_INT_ENABLED {0x00;}
sub AUDIO_INIT_BITS_INT_DISABLED {0x04;}
sub AUDIO_INIT_BITS_CLOCK_DIVIDE_2 {0x00;}
sub AUDIO_INIT_BITS_CLOCK_DIVIDE_1 {0x08;}
sub AUDIO_INIT_BITS_CLOCK_DIVIDE_4 {0x10;}
sub AUDIO_INIT_BITS_CLOCK_DIVIDE_3 {0x20;}
sub AUDIO_INIT_BITS_RECEIVE_ABORT {0x40;}
sub AUDIO_INIT_BITS_TRANSMIT_ABORT {0x80;}
sub AUDIO_MUX_PORT_NONE {0x00;}
sub AUDIO_MUX_PORT_B1 {0x01;}
sub AUDIO_MUX_PORT_B2 {0x02;}
sub AUDIO_MUX_PORT_BA {0x03;}
sub AUDIO_MUX_PORT_BB {0x04;}
sub AUDIO_MUX_PORT_BC {0x05;}
sub AUDIO_MUX_PORT_BD {0x06;}
sub AUDIO_MUX_PORT_BE {0x07;}
sub AUDIO_MUX_PORT_BF {0x08;}
sub AUDIO_MUX_MCR4_BITS_INT_ENABLE {0x08;}
sub AUDIO_MUX_MCR4_BITS_INT_DISABLE {0x00;}
sub AUDIO_MUX_MCR4_BITS_REVERSE_BB {0x10;}
sub AUDIO_MUX_MCR4_BITS_REVERSE_BC {0x20;}
sub AUDIO_MMR1_BITS_A_LAW {0x01;}
sub AUDIO_MMR1_BITS_u_LAW {0x00;}
sub AUDIO_MMR1_BITS_LOAD_GX {0x02;}
sub AUDIO_MMR1_BITS_LOAD_GR {0x04;}
sub AUDIO_MMR1_BITS_LOAD_GER {0x08;}
sub AUDIO_MMR1_BITS_LOAD_X {0x10;}
sub AUDIO_MMR1_BITS_LOAD_R {0x20;}
sub AUDIO_MMR1_BITS_LOAD_STG {0x40;}
sub AUDIO_MMR1_BITS_LOAD_DLB {0x80;}
sub AUDIO_MMR2_BITS_AINA {0x00;}
sub AUDIO_MMR2_BITS_AINB {0x01;}
sub AUDIO_MMR2_BITS_EAR {0x00;}
sub AUDIO_MMR2_BITS_LS {0x02;}
sub AUDIO_MMR2_BITS_DTMF {0x04;}
sub AUDIO_MMR2_BITS_TONE {0x08;}
sub AUDIO_MMR2_BITS_RINGER {0x10;}
sub AUDIO_MMR2_BITS_HIGH_PASS {0x20;}
sub AUDIO_MMR2_BITS_AUTOZERO {0x40;}
sub ir { &cr;}
sub bbrb { &bbtb;}
sub bcrb { &bctb;}
1;