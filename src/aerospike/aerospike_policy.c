/*
 *
 * Copyright (C) 2014-2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#include "php.h"

#include "aerospike/aerospike_key.h"
#include "aerospike/as_config.h"
#include "aerospike/as_policy.h"
#include "aerospike/as_status.h"
#include "aerospike/as_scan.h"

#include "aerospike_common.h"
#include "aerospike_policy.h"

#define NESTING_DEPTH_PHP_INI INI_STR("aerospike.nesting_depth") ? atoi(INI_STR("aerospike.nesting_depth")) : 0
#define CONNECT_TIMEOUT_PHP_INI INI_STR("aerospike.connect_timeout") ? (uint32_t) atoi(INI_STR("aerospike.connect_timeout")) : 0
#define READ_TIMEOUT_PHP_INI INI_STR("aerospike.read_timeout") ? (uint32_t) atoi(INI_STR("aerospike.read_timeout")) : 0
#define WRITE_TIMEOUT_PHP_INI INI_STR("aerospike.write_timeout") ? (uint32_t) atoi(INI_STR("aerospike.write_timeout")) : 0
#define LOG_PATH_PHP_INI INI_STR("aerospike.log_path") ? INI_STR("aerospike.log_path") : NULL
#define LOG_LEVEL_PHP_INI INI_STR("aerospike.log_level") ? INI_STR("aerospike.log_level") : NULL
#define SERIALIZER_PHP_INI(serializer_ini)                     \
	char * serializer_str = INI_STR("aerospike.serializer");   \
	if (!serializer_str) {                                     \
		serializer_ini = SERIALIZER_NONE;                      \
	} else if (!strncmp(serializer_str, "none", 4)) {          \
		serializer_ini = SERIALIZER_NONE;                      \
	} else if (!strncmp(serializer_str, "php", 3)) {           \
		serializer_ini = SERIALIZER_PHP;                       \
	} else if (!strncmp(serializer_str, "user", 4)) {          \
		serializer_ini = SERIALIZER_USER;                      \
	} else {                                                   \
		serializer_ini = SERIALIZER_NONE;                      \
	}                                                          \

#define KEY_POLICY_PHP_INI INI_STR("aerospike.key_policy") ? (uint32_t) atoi(INI_STR("aerospike.key_policy")) : 0
#define GEN_POLICY_PHP_INI INI_STR("aerospike.key_gen") ? (uint32_t) atoi(INI_STR("aerospike.key_gen")) : 0
#define USE_BATCH_DIRECT_PHP_INI INI_STR("aerospike.use_batch_direct") ? (bool) atoi(INI_STR("aerospike.use_batch_direct")) : 0
#define COMPRESSION_THRESHOLD_PHP_INI INI_STR("aerospike.compression_threshold") ? (uint32_t) atoi(INI_STR("aerospike.compression_threshold")) : 0

/*
 *******************************************************************************************************
 * MACROS TO BE USED TO SET/IDENTIFY THE ACTUAL OPTIONAL POLICIES PASSED IN BY
 * THE USER. NEEDS TO BE IDENTIFIED TO SET THE REMNANT POLICIES USING INI
 * DEFAULTS.
 * BIT 0 => TIMEOUT
 * BIT 1 => POLICY_KEY
 *
 * Usage:
 * uint8_t options_passed_for_write = 0x0;
 *
 * Setting appropriate bits:
 *
 * In case options passed by user contains OPT_WRITE_TIMEOUT, set this:
 *      options_passed_for_write |= SET_BIT_OPT_TIMEOUT;
 * In case options passed by user contains OPT_POLICY_KEY, set this:
 *      options_passed_for_write |= SET_BIT_OPT_POLICY_KEY;
 *
 * Identifying the set bits:
 *
 * if (options_passed_for_write & SET_BIT_OPT_TIMEOUT == 0x0) {
 *      // write timeout not set by user; set using default ini value.
 * } else {
 *      // write_timeout is set by user;
 * }
 *******************************************************************************************************
 */
#define SET_BIT_OPT_TIMEOUT 0x00000001
#define SET_BIT_OPT_POLICY_KEY 0x00000010
#define SET_BIT_OPT_POLICY_GEN 0x00000100

/*
 *******************************************************************************************************
 * Function to declare policy constants in Aerospike class.
 *
 * @param Aerospike_ce          The zend class entry for Aerospike class.
 *
 * @return AEROSPIKE_OK if the success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern
as_status declare_policy_constants_php(zend_class_entry *Aerospike_ce TSRMLS_DC)
{
	int32_t i;
	as_status   status = AEROSPIKE_OK;

	if (!Aerospike_ce) {
		status = AEROSPIKE_ERR_CLIENT;
		goto exit;
	}

	for (i = 0; i <= AEROSPIKE_CONSTANTS_ARR_SIZE; i++) {
		zend_declare_class_constant_long(
		Aerospike_ce, aerospike_constants[i].constant_str,
		strlen(aerospike_constants[i].constant_str),
		aerospike_constants[i].constantno TSRMLS_CC);
	}

	zend_declare_class_constant_stringl (Aerospike_ce, "JOB_QUERY", strlen("JOB_QUERY"), "query", strlen("query") TSRMLS_CC);
	zend_declare_class_constant_stringl (Aerospike_ce, "JOB_SCAN", strlen("JOB_SCAN"), "scan", strlen("scan") TSRMLS_CC);

exit:
	return status;
}

/*
 *******************************************************************************************************
 * Function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param options_p             The optional parameters.
 * @param generation_value_p    The generation value to be set into put record.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *******************************************************************************************************
 */
extern void
get_generation_value(zval* options_p, uint16_t* generation_value_p, as_error *error_p TSRMLS_DC)
{
	DECLARE_ZVAL_P(gen_value_pp);
	DECLARE_ZVAL_P(gen_policy_pp);

	if (options_p) {
#if PHP_VERSION_ID < 70000
		if (zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_POLICY_GEN, (void **) &gen_policy_pp) == FAILURE) {
#else
		if ((gen_policy_pp = zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_POLICY_GEN)) == NULL) {
#endif
			//error_p->code = AEROSPIKE_ERR_CLIENT;
			goto exit;
		}
		if (AEROSPIKE_Z_TYPE_P(gen_policy_pp) != IS_ARRAY) {
			error_p->code = AEROSPIKE_ERR_PARAM;
			goto exit;
		}

#if PHP_VERSION_ID < 70000
		zend_hash_index_find(Z_ARRVAL_P(*gen_policy_pp), 1, (void **) &gen_value_pp);
#else
		//gen_value_pp = zend_hash_index_find(Z_ARRVAL_P(gen_policy_pp), OPT_TTL);
		gen_value_pp = zend_hash_index_find(Z_ARRVAL_P(gen_policy_pp), 1);
#endif

		if (gen_value_pp && (AEROSPIKE_Z_TYPE_P(gen_value_pp) != IS_LONG)) {
			  error_p->code = AEROSPIKE_ERR_PARAM;
			goto exit;
		}
		if (gen_value_pp) {
			*generation_value_p = AEROSPIKE_Z_LVAL_P(gen_value_pp);
		}
	}
exit:
	return;

}

/*
 *******************************************************************************************************
 * Function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param options_p             The optional parameters.
 * @param generation_value_p    The generation value to be set into put record.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *******************************************************************************************************
 */
extern as_status
get_options_ttl_value(zval* options_p, uint32_t* ttl_value_p, as_error *error_p TSRMLS_DC)
{
	DECLARE_ZVAL_P(ttl_value_pp);

	if (options_p) {
#if PHP_VERSION_ID < 70000
		if (zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_TTL, (void **) &ttl_value_pp) == FAILURE) {
#else
		if ((ttl_value_pp = zend_hash_index_find(Z_ARRVAL_P(options_p), OPT_TTL)) == NULL) {
#endif
			//error_p->code = AEROSPIKE_ERR_CLIENT;
			goto exit;
		}
		if (AEROSPIKE_Z_TYPE_P(ttl_value_pp) != IS_LONG) {
			DEBUG_PHP_EXT_DEBUG("OPT_TTL should be of type integer");
			PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
				"OPT_TTL should be of type integer");
			goto exit;
		}

		if (ttl_value_pp) {
			*ttl_value_p = AEROSPIKE_Z_LVAL_P(ttl_value_pp);
		}
	}

exit:
	return error_p->code;
}

/*
 *******************************************************************************************************
 * Function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param as_config_p           The as_config object to be passed in case of connect.
 * @param read_policy_p         The as_policy_read to be passed in case of connect/get.
 * @param write_policy_p        The as_policy_write to be passed in case of connect/put.
 * @param operate_policy_p      The as_policy_operate to be passed in case of operations:
 *                              append, prepend, increment and touch.
 * @param remove_policy_p       The as_policy_remove to be passed in case of remove.
 * @param info_policy_p         The as_policy_info to be passed in case of
 *                              scan_info, register, deregister, get_registered,
 *                              list_registered udfs.
 * @param scan_policy_p         The as_policy_scan to be passed in case of scan
 *                              and scanApply.
 * @param query_policy_p        The as_policy_query to be passed in case of
 *                              as_query_for_each.
 * @param serializer_policy_p   The serialization policy to be passed in case of put.
 * @param batch_policy_p        The as_policy_batch to be passed in case of getMany
 *                              and existsMany.
 * @param apply_policy_p        The as_policy_apply to be passed in case of udf apply.
 * @param admin_policy_p        The as_policy_admin to be passed in case of security APIs.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
static  void
set_policy_ex(as_config *as_config_p,
	as_policy_read *read_policy_p,
	as_policy_write *write_policy_p,
	as_policy_operate *operate_policy_p,
	as_policy_remove *remove_policy_p,
	as_policy_info *info_policy_p,
	as_policy_scan *scan_policy_p,
	as_policy_query *query_policy_p,
	int8_t *serializer_policy_p,
	as_scan* as_scan_p,
	as_policy_batch *batch_policy_p,
	as_policy_apply *apply_policy_p,
	as_policy_admin *admin_policy_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	//int16_t             serializer_flag = 0;

	if ((!read_policy_p) && (!write_policy_p) &&
		(!operate_policy_p) && (!remove_policy_p) && (!info_policy_p) &&
		(!scan_policy_p) && (!query_policy_p) && (!serializer_policy_p)
		&& (!batch_policy_p) && (!apply_policy_p) && (!admin_policy_p)) {
		DEBUG_PHP_EXT_DEBUG("Unable to set policy");
		PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT, "Unable to set policy");
		goto exit;
	}

	/*
	 * case: connect => (read_policy_p != NULL && write_policy_p != NULL)
	 */

	if (read_policy_p && (!write_policy_p)) {
		/*
		 * case: get
		 */
		as_policy_read_init(read_policy_p);
		as_policy_read_copy(&as_config_p->policies.read,
				read_policy_p);
	} else if (write_policy_p && (!read_policy_p)) {
		/*
		 * case: put
		 */
		as_policy_write_init(write_policy_p);
		as_policy_write_copy(&as_config_p->policies.write,
				write_policy_p);
	} else if (operate_policy_p) {
		/*
		 * case: operate
		 */
		as_policy_operate_init(operate_policy_p);
		as_policy_operate_copy(&as_config_p->policies.operate,
				operate_policy_p);
	} else if (remove_policy_p) {
		/*
		 * case: remove
		 */
		as_policy_remove_init(remove_policy_p);
		as_policy_remove_copy(&as_config_p->policies.remove,
				remove_policy_p);
	} else if (info_policy_p) {
		/*
		 * case: info
		 */
		as_policy_info_init(info_policy_p);
		as_policy_info_copy(&as_config_p->policies.info,
				info_policy_p);
	} else if (scan_policy_p) {
		/*
		 * case: scan, scanApply
		 */
		as_policy_scan_init(scan_policy_p);
		as_policy_scan_copy(&as_config_p->policies.scan,
				scan_policy_p);
	} else if (query_policy_p) {
		/*
		 * case: query, aggregate
		 */
		as_policy_query_init(query_policy_p);
		as_policy_query_copy(&as_config_p->policies.query,
				query_policy_p);
	} else if (batch_policy_p) {
		/*
		 * case: getMany, existsMany
		 */
		as_policy_batch_init(batch_policy_p);
		as_policy_batch_copy(&as_config_p->policies.batch,
				batch_policy_p);
	} else if(apply_policy_p) {
		/*
		 * case : apply udf
		 */
		as_policy_apply_init(apply_policy_p);
		as_policy_apply_copy(&as_config_p->policies.apply,
				apply_policy_p);
	} else if(admin_policy_p) {
		/*
		 * case : security APIs
		 */
		as_policy_admin_init(admin_policy_p);
	}

	if (options_p != NULL) {
		HashTable*          options_array = Z_ARRVAL_P(options_p);
		HashPosition        options_pointer;
		int                 scan_percentage = 0;
		uint16_t            options_passed_for_write = 0x0;
		uint8_t             options_passed_for_read = 0x0;
		uint8_t             options_passed_for_operate = 0x0;
		uint8_t             options_passed_for_remove = 0x0;
		zval*               gen_policy_p = NULL;
		DECLARE_ZVAL_P(gen_policy_pp);
		DECLARE_ZVAL_P(options_value);
#if PHP_VERSION_ID < 70000
		int8_t*             options_key;
#else
		zend_string*        options_key;
#endif

		uint options_key_len;
#if PHP_VERSION_ID < 70000
		AEROSPIKE_FOREACH_HASHTABLE(options_array, options_pointer, options_value) {
			ulong options_index;
			if (HASH_KEY_IS_LONG != AEROSPIKE_ZEND_HASH_GET_CURRENT_KEY_EX(options_array, &options_key,
						&options_key_len, &options_index, 0, &options_pointer)) {
				DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
				PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Policy Constant Key");
				goto exit;
			}
#else
			ZEND_HASH_FOREACH_VAL(options_array, options_value) {
			zend_ulong options_index;
			ZEND_HASH_FOREACH_KEY_VAL(options_array, options_index, options_key, options_value) {
			if (options_key) {
				DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
				PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Policy Constant Key");
				goto exit;
			}
		} ZEND_HASH_FOREACH_END();
#endif
		switch((int) options_index) {
			case OPT_CONNECT_TIMEOUT:
				if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
					goto exit;
				}
				as_config_p->conn_timeout_ms = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				break;
			case OPT_READ_TIMEOUT:
				if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
					goto exit;
				}
				if (read_policy_p) {
					read_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (info_policy_p) {
					info_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (scan_policy_p) {
					scan_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (query_policy_p) {
					query_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (batch_policy_p) {
					batch_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
					goto exit;
				}
				break;
			case OPT_WRITE_TIMEOUT:
				if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
					goto exit;
				}
				if (write_policy_p) {
					write_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if(operate_policy_p) {
					operate_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					options_passed_for_operate |= SET_BIT_OPT_TIMEOUT;
				} else if(remove_policy_p) {
					remove_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					options_passed_for_remove |= SET_BIT_OPT_TIMEOUT;
				} else if(info_policy_p) {
					info_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if(scan_policy_p) {
					scan_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if(query_policy_p) {
					query_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (apply_policy_p) {
					apply_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else if (admin_policy_p) {
					admin_policy_p->timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
				} else {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
					goto exit;
				}
				break;
			case OPT_POLICY_EXISTS:
				if ((!write_policy_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
					goto exit;
				}
				write_policy_p->exists = AEROSPIKE_Z_LVAL_P(options_value);
				break;
			case OPT_POLICY_RETRY:
				if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
					goto exit;
				}
				if (write_policy_p) {
					write_policy_p->retry = AEROSPIKE_Z_LVAL_P(options_value);
				} else if(operate_policy_p) {
					operate_policy_p->retry = AEROSPIKE_Z_LVAL_P(options_value);
				} else if(remove_policy_p) {
					remove_policy_p->retry = AEROSPIKE_Z_LVAL_P(options_value);
				} else {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
					goto exit;
				}
				break;
			case OPT_SERIALIZER:
				if ((!serializer_policy_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SERIALIZER");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Value for OPT_SERIALIZER");
					goto exit;
				}
				*serializer_policy_p = AEROSPIKE_Z_LVAL_P(options_value);
				//serializer_flag = 1;
				break;
			case OPT_SCAN_PRIORITY:
				 if (info_policy_p) {
					 break;
				 }
			   if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_PRIORITY");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Value for OPT_SCAN_PRIORITY");
					goto exit;
				}
				if (!as_scan_set_priority(as_scan_p, (uint32_t) AEROSPIKE_Z_LVAL_P(options_value))) {
					DEBUG_PHP_EXT_DEBUG("Unable to set scan priority");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM, "Unable to set scan priority");
					goto exit;
				}
				break;
			case OPT_SCAN_PERCENTAGE:
				  if (info_policy_p) {
					  break;
				  }
				  if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
					  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_PERCENTAGE");
					  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							  "Unable to set policy: Invalid Value for OPT_SCAN_PERCENTAGE");
					  goto exit;
				  }
				  scan_percentage = AEROSPIKE_Z_LVAL_P(options_value);
				  if (scan_percentage < 0 || scan_percentage > 100) {
					  DEBUG_PHP_EXT_DEBUG("Invalid value for scan percent");
					  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM, "Invalid value for scan percent");
					  goto exit;
				  } else if (!as_scan_set_percent(as_scan_p, scan_percentage)) {
					  DEBUG_PHP_EXT_DEBUG("Unable to set scan percent");
					  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM, "Unable to set scan percent");
					  goto exit;
				  }
				  break;
			case OPT_SCAN_CONCURRENTLY:
					  if (info_policy_p) {
						  break;
					  }
					  #if PHP_VERSION_ID < 70000
						if ((!as_scan_p) || ((AEROSPIKE_Z_TYPE_P(options_value) != IS_BOOL))) {
					  #else
						if ((!as_scan_p) || ((AEROSPIKE_Z_TYPE_P(options_value) != IS_TRUE && AEROSPIKE_Z_TYPE_P(options_value) != IS_FALSE))) {
					  #endif
						  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_CONCURRENTLY");
						  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								  "Unable to set policy: Invalid Value for OPT_SCAN_CONCURRENTLY");
						  goto exit;
					  }
					  #if PHP_VERSION_ID < 70000
						if (!as_scan_set_concurrent(as_scan_p, (uint32_t) Z_BVAL_PP(options_value))) {
					  #else
						if (!as_scan_set_concurrent(as_scan_p, (uint32_t) Z_DVAL_P(options_value))) {
					  #endif
						  DEBUG_PHP_EXT_DEBUG("Unable to set scan concurrency");
						  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM, "Unable to set scan concurrency");
						  goto exit;
					  }
					  break;
			case OPT_SCAN_NOBINS:
						  if (info_policy_p) {
							  break;
						  }
						  #if PHP_VERSION_ID < 70000
							if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_BOOL)) {
						  #else
							if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_TRUE && AEROSPIKE_Z_TYPE_P(options_value) != IS_FALSE)) {
						  #endif
							  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_NOBINS");
							  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
									  "Unable to set policZ_BVAL_PPy: Invalid Value for OPT_SCAN_NOBINS");
							  goto exit;
						  }
						  #if PHP_VERSION_ID < 70000
							if (!as_scan_set_nobins(as_scan_p, (uint32_t) Z_BVAL_PP(options_value))) {
						  #else
							if (!as_scan_set_nobins(as_scan_p, (uint32_t) Z_DVAL_P(options_value))) {
						  #endif
							  DEBUG_PHP_EXT_DEBUG("Unable to set scan no bins");
							  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM, "Unable to set scan no bins");
							  goto exit;
						  }
						  break;
			case OPT_SCAN_INCLUDELDT:
							  if (info_policy_p) {
								  break;
							  }
							  #if PHP_VERSION_ID < 70000
								if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_BOOL)) {
							  #else
								if ((!as_scan_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_TRUE && AEROSPIKE_Z_TYPE_P(options_value) != IS_FALSE)) {
							  #endif
								  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SCAN_INCLUDELDT");
								  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
										  "Unable to set policy: Invalid Value for OPT_SCAN_INCLUDELDT");
								  goto exit;
							  }
							  #if PHP_VERSION_ID < 70000
								as_scan_p->include_ldt = Z_BVAL_PP(options_value);
							  #else
								as_scan_p->include_ldt = Z_DVAL_P(options_value);
							  #endif
							  break;
			case OPT_POLICY_KEY:
								  if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
									  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_KEY");
									  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
											  "Unable to set policy: Invalid Value for OPT_POLICY_KEY");
									  goto exit;
								  }
								  if (write_policy_p) {
									  write_policy_p->key = AEROSPIKE_Z_LVAL_P(options_value);
								  } else if (read_policy_p) {
									  read_policy_p->key = AEROSPIKE_Z_LVAL_P(options_value);
								  } else if (operate_policy_p) {
									  operate_policy_p->key = AEROSPIKE_Z_LVAL_P(options_value);
								  } else if (remove_policy_p) {
									  remove_policy_p->key = AEROSPIKE_Z_LVAL_P(options_value);
								  } else if (apply_policy_p) {
									  apply_policy_p->key = AEROSPIKE_Z_LVAL_P(options_value);
								  } else {
									  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_KEY");
									  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
											  "Unable to set policy: Invalid Value for OPT_POLICY_KEY");
									  goto exit;
								  }
								  break;
			case OPT_POLICY_GEN:
								  #if PHP_VERSION_ID < 70000
								  zend_hash_index_find(Z_ARRVAL_P(*options_value), 0, (void **) &gen_policy_pp);

								if (AEROSPIKE_Z_TYPE_P(gen_policy_pp) != IS_LONG) {
									DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_GEN");
									PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
											"Unable to set policy: Invalid Value for OPT_POLICY_GEN");
									goto exit;
								}
								if (write_policy_p) {
									write_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_pp);
								} else if (operate_policy_p) {
									operate_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_pp);
								} else if (remove_policy_p) {
									remove_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_pp);
								  #else
								  if (NULL == (gen_policy_p = AEROSPIKE_ZEND_HASH_INDEX_FIND(Z_ARRVAL_P(options_value), 0,
											&gen_policy_p))) {
										goto exit;
									}

									if (AEROSPIKE_Z_TYPE_P(gen_policy_p) != IS_LONG) {
										DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_GEN");
										PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
												"Unable to set policy: Invalid Value for OPT_POLICY_GEN");
										goto exit;
									}
									if (write_policy_p) {
										write_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_p);
									} else if (operate_policy_p) {
										operate_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_p);
									} else if (remove_policy_p) {
										remove_policy_p->gen = AEROSPIKE_Z_LVAL_P(gen_policy_p);
								  #endif

									  } else {
										  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_GEN");
										  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
												  "Unable to set policy: Invalid Value for OPT_POLICY_GEN");
										  goto exit;
									  }
									  break;


			case OPT_POLICY_COMMIT_LEVEL:
			  if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
				  goto exit;
			  }
			  if (write_policy_p) {
				  write_policy_p->commit_level = AEROSPIKE_Z_LVAL_P(options_value);
			  } else if (operate_policy_p) {
				  operate_policy_p->commit_level = AEROSPIKE_Z_LVAL_P(options_value);
			  } else if (remove_policy_p) {
				  remove_policy_p->commit_level = AEROSPIKE_Z_LVAL_P(options_value);
			  } else {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
				  goto exit;
			  }
			  break;
		  case OPT_POLICY_CONSISTENCY:
			  if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
				  goto exit;
			  }
			  if (read_policy_p) {
				  read_policy_p->consistency_level = AEROSPIKE_Z_LVAL_P(options_value);
			  } else if (operate_policy_p) {
				  operate_policy_p->consistency_level = AEROSPIKE_Z_LVAL_P(options_value);
			  } else {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
				  goto exit;
			  }
			  break;
		  case OPT_POLICY_REPLICA:
			  if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
				  goto exit;
			  }
			  if (read_policy_p) {
				  read_policy_p->replica = AEROSPIKE_Z_LVAL_P(options_value);
			  } else if (operate_policy_p) {
				  operate_policy_p->replica = AEROSPIKE_Z_LVAL_P(options_value);
			  } else {
				  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
				  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						  "Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
				  goto exit;
			  }
			  break;

		  case OPT_TTL:
			  break;
		  default:
			  DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
			  PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
					  "Unable to set policy: Invalid Policy Constant Key");
			  goto exit;
	  }
#if PHP_VERSION_ID < 70000
}
#else
} ZEND_HASH_FOREACH_END();
#endif


}

/*
if (serializer_flag == 0) {
  int8_t serializer_int = 0;
  SERIALIZER_PHP_INI(serializer_int);
  if (serializer_policy_p) {
	  *serializer_policy_p = serializer_int;
  }
}
*/

PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_OK, DEFAULT_ERROR);
exit:
	return;
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the read policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param read_policy_p         The as_policy_read to be passed in case of get.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_read(as_policy_read *read_policy_p, zval *options_p, as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, read_policy_p, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the write policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param write_policy_p        The as_policy_write to be passed in case of put.
 * @param serializer_policy_p   The integer serializer_policy to be passed to
 *                              handle aerospike-unsupported data types in put.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_write(as_policy_write *write_policy_p,
	int8_t *serializer_policy_p, zval *options_p, as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, NULL, write_policy_p, NULL, NULL, NULL, NULL,
		NULL, serializer_policy_p, NULL, NULL, NULL, NULL, options_p,
		error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the operate policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param operate_policy_p      The as_policy_operate to be passed in case of operate.
 * @param serializer_policy_p   The integer serializer_policy to be passed to
 *                              handle aerospike-unsupported data types in operate.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_operate(as_policy_operate *operate_policy_p,
	int8_t *serializer_policy_p, zval *options_p, as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, NULL, NULL, operate_policy_p, NULL, NULL, NULL,
			NULL, serializer_policy_p, NULL, NULL, NULL, NULL,
			options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the remove policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param remove_policy_p       The as_policy_remove to be passed in case of remove.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_remove(as_policy_remove *remove_policy_p, zval *options_p, as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, NULL, NULL, NULL, remove_policy_p, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the info policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param info_policy_p         The as_policy_info to be passed in case of scan get info,
 *                              udf operations, index operations, info operations.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_info(as_policy_info *info_policy_p, zval *options_p, as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, NULL, NULL, NULL, NULL, info_policy_p, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the query policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param query_policy_p        The as_policy_query to be passed in case of
 *                              query/aggregate.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy(as_config *as_config_p,
		as_policy_read *read_policy_p,
		as_policy_write *write_policy_p,
		as_policy_operate *operate_policy_p,
		as_policy_remove *remove_policy_p,
		as_policy_info *info_policy_p,
		as_policy_scan *scan_policy_p,
		as_policy_query *query_policy_p,
		int8_t *serializer_policy_p,
		zval *options_p,
		as_error *error_p TSRMLS_DC)
{
	set_policy_ex(as_config_p, read_policy_p, write_policy_p, operate_policy_p,
		remove_policy_p, info_policy_p, scan_policy_p, query_policy_p,
		serializer_policy_p, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the scan policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param scan_policy_p         The as_policy_scan to be passed in case of scan.
 * @param serializer_policy_p   The integer serializer_policy to be passed to
 *                              handle aerospike-unsupported data types in operate.
 * @param as_scan_p             The as_scan object to be set.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_scan(as_config *as_config_p,
	as_policy_scan *scan_policy_p,
	int8_t *serializer_policy_p,
	as_scan *as_scan_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	set_policy_ex(as_config_p, NULL, NULL, NULL, NULL, NULL, scan_policy_p, NULL,
		serializer_policy_p, as_scan_p, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}


/*
 *******************************************************************************************************
 * Wrapper function for setting the queryApply policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param write_policy_p        The as_policy_write to be passed in case of
 *                              batch operations.
 * @param options_p             The user's optional policy options to be used if
 *                              set, else default.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_query_apply(as_config *as_config_p,
	as_policy_write *write_policy_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	set_policy_ex(as_config_p, NULL, write_policy_p, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, options_p, error_p TSRMLS_CC);
}


/*
 *******************************************************************************************************
 * Wrapper function for setting the batch policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param batch_policy_p        The as_policy_batch to be passed in case of batch oeprations.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_batch(as_config *as_config_p,
	as_policy_batch *batch_policy_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	set_policy_ex(as_config_p, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, batch_policy_p, NULL, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the apply policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param apply_policy_p        The as_policy_apply to be passed in case of udf apply.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_udf_apply(as_config *as_config_p,
	as_policy_apply *apply_policy_p,
	int8_t *serializer_policy_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	set_policy_ex(as_config_p, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		serializer_policy_p, NULL, NULL, apply_policy_p, NULL, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the admin policy by using the user's
 * optional policy options (if set) else the defaults.
 *
 * @param admin_policy_p        The as_policy_admin to be passed in case of security operations.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 *
 *******************************************************************************************************
 */
extern void
set_policy_admin(as_policy_admin *admin_policy_p,
	zval *options_p,
	as_error *error_p TSRMLS_DC)
{
	set_policy_ex(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, admin_policy_p, options_p, error_p TSRMLS_CC);
}

/*
 *******************************************************************************************************
 * Function for checking and setting the default aerospike policies by reading
 * from php.ini entries if configured by user, else the global defaults.
 *
 * @param as_config_p           The as_config object to be passed in case of connect.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 * @param serializer_opt        The serializer option to be set in AerospikeObject structure.
 *                              Value will be read from INI and then from user's option array
 *                              if provided.
 *
 *******************************************************************************************************
 */
void
set_config_policies(as_config *as_config_p,
	zval *options_p,
	as_error *error_p,
	int8_t *serializer_opt TSRMLS_DC)
{
	/*
	 * Copy INI values to global_config_policy
	 */

	int32_t ini_value = 0;

	ini_value = READ_TIMEOUT_PHP_INI;
	if (ini_value && as_config_p) {
		as_config_p->policies.read.timeout = ini_value;
		as_config_p->policies.info.timeout = ini_value;
		as_config_p->policies.batch.timeout = ini_value;
		as_config_p->policies.scan.timeout = ini_value;
		as_config_p->policies.query.timeout = ini_value;
	}

	ini_value = KEY_POLICY_PHP_INI;
	if (ini_value && as_config_p) {
		as_config_p->policies.read.key = ini_value;
		as_config_p->policies.write.key = ini_value;
		as_config_p->policies.operate.key = ini_value;
		as_config_p->policies.remove.key = ini_value;
	}

	ini_value = WRITE_TIMEOUT_PHP_INI;
	if (ini_value && as_config_p) {
		as_config_p->policies.write.timeout = ini_value;
		as_config_p->policies.operate.timeout = ini_value;
		as_config_p->policies.remove.timeout = ini_value;
		as_config_p->policies.apply.timeout = ini_value;
	}


	ini_value = CONNECT_TIMEOUT_PHP_INI;

	if (ini_value && as_config_p) {
		as_config_p->conn_timeout_ms = ini_value;
	}

	SERIALIZER_PHP_INI(ini_value);

	if (serializer_opt){
		*serializer_opt = ini_value;
	}

	ini_value = USE_BATCH_DIRECT_PHP_INI;
	if (as_config_p) {
		as_config_p->policies.batch.use_batch_direct = ini_value;
	}

	ini_value = COMPRESSION_THRESHOLD_PHP_INI;
	if (ini_value >= 0 && as_config_p) {
		as_config_p->policies.write.compression_threshold = ini_value;
	}

	if (options_p != NULL) {
		HashTable*			options_array = Z_ARRVAL_P(options_p);
		HashPosition		options_pointer;
		DECLARE_ZVAL_P(options_value);
#if PHP_VERSION_ID < 70000
		int8_t*				options_key;
		AEROSPIKE_FOREACH_HASHTABLE(options_array, options_pointer, options_value) {
			ulong options_index;
#else
		zend_string*		options_key;
		ZEND_HASH_FOREACH_VAL(options_array, options_value) {
			zend_ulong options_index;
#endif
			uint options_key_len;

#if PHP_VERSION_ID < 70000
			if (HASH_KEY_IS_LONG != AEROSPIKE_ZEND_HASH_GET_CURRENT_KEY_EX(options_array, &options_key,
				&options_key_len, &options_index, 0, &options_pointer)) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Policy Constant Key");
					goto exit;
			}
#else
			ZEND_HASH_FOREACH_KEY_VAL(options_array, options_index, options_key, options_value) {
				if (options_key) {
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
						"Unable to set policy: Invalid Policy Constant Key");
					goto exit;
				}
			} ZEND_HASH_FOREACH_END();
#endif
			switch((int) options_index) {
				case OPT_CONNECT_TIMEOUT:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
							"Unable to set policy: Invalid Value for OPT_CONNECT_TIMEOUT");
						goto exit;
					}
					as_config_p->conn_timeout_ms = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case OPT_READ_TIMEOUT:
					if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_READ_TIMEOUT");
						goto exit;
					}
					uint32_t read_timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					as_config_p->policies.read.timeout = read_timeout;
					as_config_p->policies.info.timeout = read_timeout;
					as_config_p->policies.batch.timeout = read_timeout;
					as_config_p->policies.scan.timeout = read_timeout;
					as_config_p->policies.query.timeout = read_timeout;
					break;
				case OPT_WRITE_TIMEOUT:
					if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_WRITE_TIMEOUT");
						goto exit;
					}
					uint32_t write_timeout = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					as_config_p->policies.write.timeout = write_timeout;
					as_config_p->policies.operate.timeout = write_timeout;
					as_config_p->policies.remove.timeout = write_timeout;
					as_config_p->policies.apply.timeout = write_timeout;
					break;
				case OPT_POLICY_KEY:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_KEY");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_KEY");
						goto exit;
					}
					uint32_t key_policy = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					as_config_p->policies.key = key_policy;
					as_config_p->policies.read.key = key_policy;
					as_config_p->policies.write.key = key_policy;
					as_config_p->policies.operate.key = key_policy;
					as_config_p->policies.remove.key = key_policy;
					break;
				case OPT_POLICY_RETRY:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_RETRY");
						goto exit;
					}
					as_config_p->policies.retry = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case OPT_POLICY_EXISTS:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_EXISTS");
						goto exit;
					}
					as_config_p->policies.exists = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case OPT_POLICY_REPLICA:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_REPLICA");
						goto exit;
					}
					as_config_p->policies.replica = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case OPT_POLICY_CONSISTENCY:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_CONSISTENCY");
						goto exit;
					}
					as_config_p->policies.consistency_level = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case OPT_POLICY_COMMIT_LEVEL:
					if ((!as_config_p) || (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_POLICY_COMMIT_LEVEL");
						goto exit;
					}
					as_config_p->policies.commit_level = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
					break;
				 case OPT_SERIALIZER:
					if ((!serializer_opt) && (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG)) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for OPT_SERIALIZER");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Value for OPT_SERIALIZER");
						goto exit;
					}
					*serializer_opt = (int8_t)AEROSPIKE_Z_LVAL_P(options_value);
					break;
				case USE_BATCH_DIRECT:
#if PHP_VERSION_ID < 70000
					if ((Z_TYPE_PP(options_value) != IS_BOOL)) {
#else
					if ((Z_TYPE_P(options_value) != IS_TRUE) && (Z_TYPE_P(options_value) != IS_FALSE)) {
#endif
						DEBUG_PHP_EXT_DEBUG("Unable to set USE_BATCH_DIRECT : Incorrect Value type for USE_BATCH_DIRECT");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set USE_BATCH_DIRECT:Incorrect Value type for USE_BATCH_DIRECT");
						goto exit;
					}
#if PHP_VERSION_ID < 70000
					as_config_p->policies.batch.use_batch_direct = (bool) Z_BVAL_PP(options_value);
#else
					as_config_p->policies.batch.use_batch_direct = (bool) Z_DVAL_P(options_value);
#endif
					break;
				case COMPRESSION_THRESHOLD:
					if (AEROSPIKE_Z_TYPE_P(options_value) != IS_LONG) {
						DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Value for COMPRESSION_THRESHOLD");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
										   "Unable to set policy: Invalid Value for COMPRESSION_THRESHOLD");
						goto exit;
					}
					uint32_t compression_threshold = (uint32_t) AEROSPIKE_Z_LVAL_P(options_value);
#if PHP_VERSION_ID < 70000
					as_config_p->policies.write.compression_threshold = compression_threshold;
#else
					if (compression_threshold >= 0) {
						as_config_p->policies.write.compression_threshold = compression_threshold;
					} else {
						DEBUG_PHP_EXT_DEBUG("Compression threshold should be >= 0");
						PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
										 "Compression threshold should be >= 0");
						goto exit;
					}
#endif
					break;
				default:
					DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid Policy Constant Key");
					PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_PARAM,
								"Unable to set policy: Invalid Policy Constant Key");
					goto exit;
			}
#if PHP_VERSION_ID < 70000
		}
#else
		} ZEND_HASH_FOREACH_END();
#endif
	}
exit:
	return;
}

/*
 *******************************************************************************************************
 * Wrapper function for setting the relevant aerospike policies by using the user's
 * optional policy options (if set) else the defaults.
 * (Called in case of connect.)
 *
 * @param as_config_p           The as_config object to be passed in case of connect.
 * @param options_p             The user's optional policy options to be used if set, else defaults.
 * @param error_p               The as_error to be populated by the function
 *                              with the encountered error if any.
 * @param serializer_opt        The serializer option to be set in AerospikeObject structure.
 *                              Value will be read from INI and then from user's option array
 *                              if provided.
 *
 *******************************************************************************************************
 */
extern void
set_general_policies(as_config *as_config_p, zval *options_p, as_error *error_p,
	int8_t *serializer_opt TSRMLS_DC)
{
	if (!as_config_p) {
		DEBUG_PHP_EXT_DEBUG("Unable to set policy: Invalid as_config");
		PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT, "Unable to set policy: Invalid as_config");
		goto exit;
	}
	set_config_policies(as_config_p, options_p, error_p, serializer_opt TSRMLS_CC);

exit:
	return;
}
