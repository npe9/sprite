if (!defined &_FSCMD) {
    eval 'sub _FSCMD {1;}';
    eval 'sub FS_PREFIX_LOAD {1;}';
    eval 'sub FS_PREFIX_EXPORT {2;}';
    eval 'sub FS_PREFIX_CLEAR {3;}';
    eval 'sub FS_PREFIX_DELETE {4;}';
    eval 'sub FS_PREFIX_CONTROL {5;}';
    eval 'sub FS_RAISE_MIN_CACHE_SIZE {6;}';
    eval 'sub FS_LOWER_MAX_CACHE_SIZE {7;}';
    eval 'sub FS_DISABLE_FLUSH {8;}';
    eval 'sub FS_SET_TRACING {9;}';
    eval 'sub FS_SET_CACHE_DEBUG {10;}';
    eval 'sub FS_SET_RPC_DEBUG {11;}';
    eval 'sub FS_SET_RPC_TRACING {12;}';
    eval 'sub FS_SET_NAME_CACHING {13;}';
    eval 'sub FS_SET_CLIENT_CACHING {14;}';
    eval 'sub FS_TEST_CS {15;}';
    eval 'sub FS_SET_RPC_CLIENT_HIST {16;}';
    eval 'sub FS_SET_RPC_SERVER_HIST {17;}';
    eval 'sub FS_EMPTY_CACHE {18;}';
    eval 'sub FS_RETURN_STATS {19;}';
    eval 'sub FS_GET_FRAG_INFO {21;}';
    eval 'sub FS_SET_NO_STICKY_SEGS {22;}';
    eval 'sub FS_SET_CLEANER_PROCS {23;}';
    eval 'sub FS_SET_READ_AHEAD {24;}';
    eval 'sub FS_SET_RA_TRACING {25;}';
    eval 'sub FS_SET_RPC_NO_TIMEOUTS {26;}';
    eval 'sub FS_SET_WRITE_THROUGH {27;}';
    eval 'sub FS_SET_WRITE_BACK_ON_CLOSE {28;}';
    eval 'sub FS_SET_DELAY_TMP_FILES {29;}';
    eval 'sub FS_SET_TMP_DIR_NUM {30;}';
    eval 'sub FS_SET_WRITE_BACK_INTERVAL {31;}';
    eval 'sub FS_SET_WRITE_BACK_ASAP {32;}';
    eval 'sub FS_SET_WB_ON_LAST_DIRTY_BLOCK {33;}';
    eval 'sub FS_SET_BLOCK_SKEW {34;}';
    eval 'sub FS_SET_LARGE_FILE_MODE {35;}';
    eval 'sub FS_SET_MAX_FILE_PORTION {36;}';
    eval 'sub FS_SET_DELETE_HISTOGRAMS {37;}';
    eval 'sub FS_ZERO_STATS {38;}';
    eval 'sub FS_SET_PDEV_DEBUG {39;}';
    eval 'sub FS_SET_MIG_DEBUG {40;}';
    eval 'sub FS_RETURN_LIFE_TIMES {41;}';
    eval 'sub FS_REREAD_SUMMARY_INFO {42;}';
    eval 'sub FS_DO_L1_COMMAND {43;}';
    eval 'sub FS_FIRST_LFS_COMMAND {100;}';
    eval 'sub FS_CLEAN_LFS_COMMAND {101;}';
    eval 'sub FS_SET_CONTROL_FLAGS_LFS_COMMAND {102;}';
    eval 'sub FS_GET_CONTROL_FLAGS_LFS_COMMAND {103;}';
    eval 'sub FS_FREE_FILE_NUMBER_LFS_COMMAND {104;}';
    eval 'sub FS_LAST_LFS_COMMAND {199;}';
}
1;
