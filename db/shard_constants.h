/*
 * ===============================================================
 *    Description:  Constants for shards.
 *
 *        Created:  2014-06-19 13:01:18
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#ifndef weaver_db_shard_constants_h_
#define weaver_db_shard_constants_h_

#define NUM_NODE_MAPS 1024
#define SHARD_MSGRECV_TIMEOUT 1 // busybee recv timeout (ms) for shard worker threads

#define BATCH_MSG_SIZE 1 // 1 == no batching

// migration
#define SHARD_CAP (90000ULL/NUM_SHARDS)
//#define WEAVER_CLDG // defined if communication-based LDG, undef otherwise
//#define WEAVER_NEW_CLDG // defined if communication-based LDG, undef otherwise
//#define WEAVER_MSG_COUNT // defined if client msg count call will take place

#endif