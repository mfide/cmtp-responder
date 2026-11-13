/*
 * Copyright (c) 2012, 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2019 Collabora Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <glib.h>
#include "mtp_support.h"
#include "mtp_util.h"
#include "ptp_datacodes.h"
#include "mtp_device.h"
#include "mtp_transport.h"
#include "ptp_container.h"
#include "mtp_thread.h"
#include "mtp_event_handler.h"
#include "mtp_inoti_handler.h"

#define MTP_DEVICE_VERSION_CHAR		"V1.0"

/*
 * GLOBAL AND EXTERN VARIABLES
 */
extern pthread_mutex_t g_cmd_inoti_mutex;
static mtp_device_t _g_device = { 0 };
mtp_device_t *g_device = &_g_device;

/*
 * STATIC VARIABLES
 */
static mtp_store_t g_store_list[MAX_NUM_DEVICE_STORES];

static mtp_uint16 g_ops_supported[] = {
	PTP_OPCODE_GETDEVICEINFO,
	PTP_OPCODE_OPENSESSION,
	PTP_OPCODE_CLOSESESSION,
	PTP_OPCODE_GETSTORAGEIDS,
	PTP_OPCODE_GETSTORAGEINFO,
	PTP_OPCODE_GETOBJECTHANDLES,
	PTP_OPCODE_GETOBJECTINFO,
	PTP_OPCODE_GETOBJECT,
	PTP_OPCODE_DELETEOBJECT,
	PTP_OPCODE_SENDOBJECTINFO,
	PTP_OPCODE_SENDOBJECT,

#ifdef MTP_SUPPORT_SET_PROTECTION
	PTP_OPCODE_SETOBJECTPROTECTION,
#endif /* MTP_SUPPORT_SET_PROTECTION */

        PTP_OPCODE_GETPARTIALOBJECT,
        MTP_OPCODE_GETOBJECTPROPSUPPORTED, // windows 11 needs this to be able to handle >4GB files
        MTP_OPCODE_GETOBJECTPROPDESC,
        MTP_OPCODE_GETOBJECTPROPVALUE,     // windows 11 needs this to be able to handle >4GB files
        MTP_OPCODE_SETOBJECTPROPVALUE,
	MTP_OPCODE_GETOBJECTPROPLIST,

/* Android Random I/O Extensions Codes which is a part of MTP protocol events
 * trigered  for file access operations */
        PTP_OC_ANDROID_GETPARTIALOBJECT,
	PTP_OPCODE_GETDEVICEPROPDESC,
	PTP_OPCODE_GETDEVICEPROPVALUE,
	PTP_OPCODE_MOVEOBJECT,
	PTP_OPCODE_COPYOBJECT,
};

static mtp_uint16 g_event_supported[] = {
	PTP_EVENTCODE_OBJECTADDED,
	PTP_EVENTCODE_OBJECTREMOVED,
	PTP_EVENTCODE_OBJECTINFOCHANGED,
	PTP_EVENTCODE_STOREADDED,
	PTP_EVENTCODE_STOREREMOVED,
	PTP_EVENTCODE_STORAGEINFOCHANGED,
	PTP_EVENTCODE_DEVICEPROPCHANGED,
};

/* filled at start: the name always, battery level and device type when the platform gives them */
static mtp_uint16 g_device_prop_supported[3];
static mtp_uint32 g_device_prop_supported_cnt;
static mtp_char g_friendly_name[128];
static const mtp_char *g_battery_file;
static mtp_uint32 g_device_type;

static mtp_uint16 g_capture_fmts[] = {
};

static mtp_uint16 g_object_fmts[] = {
	PTP_FMT_UNDEF,       // this is for any file which is not explicitly listed as PTP_FMT_xyz
	PTP_FMT_ASSOCIATION, // this is for folders
};

/*
 * static mtp_err_t __device_clear_store_data(mtp_uint32 store_id)
 * This function removes the storage entry.
 * @param[in]	store_id	Specifies the storage id to remove
 * @return	This function returns MTP_ERROR_NONE on success
 *		ERROR number on failure.
 */
static mtp_err_t __clear_store_data(mtp_uint32 store_id)
{
	mtp_uint16 idx = 0;
	mtp_store_t *store = NULL;
	mtp_uint32 count = g_device->num_stores;

	for (idx = 0; idx < count; idx++) {
		if (g_device->store_list[idx].store_id == store_id) {

			_entity_destroy_mtp_store(&(g_device->store_list[idx]));

			store = &(g_device->store_list[idx]);
			store->store_id = 0;
			store->is_hidden = FALSE;
			g_free(store->root_path);
			store->root_path = NULL;

			for (; idx < count - 1; idx++) {
				_entity_copy_store_data(&(g_device->store_list[idx]),
						&(g_device->store_list[idx + 1]));
			}

			g_device->store_list[count - 1].store_id = 0;
			g_device->store_list[count - 1].root_path = NULL;
			g_device->store_list[count - 1].is_hidden = FALSE;
			_util_init_list(&(g_device->store_list[count - 1].obj_list));

			/*Initialize the destroyed store*/
			g_device->num_stores--;
			return MTP_ERROR_NONE;
		}
	}
	return MTP_ERROR_GENERAL;
}

/* the platform names the device at run time (MTP_MANUFACTURER, MTP_MODEL, MTP_VERSION, MTP_SERIAL) */
static const char *__env_or(const char *name, const char *fallback)
{
	const char *v = getenv(name);

	return v && v[0] ? v : fallback;
}

void _init_mtp_device(void)
{
	device_info_t *info = &(g_device->device_info);
	mtp_wchar wtemp[MAX_PTP_STRING_CHARS + 1] = { 0 };

	g_device->status = DEVICE_STATUSOK;
	g_device->phase = DEVICE_PHASE_IDLE;
	g_device->num_stores = 0;
	g_device->store_list = g_store_list;
	g_device->default_store_id = MTP_EXTERNAL_STORE_ID;
	g_device->default_hparent = PTP_OBJECTHANDLE_ROOT;

	/* the name the PC shows: MTP_FRIENDLY_NAME from the platform, else the model */
	g_strlcpy(g_friendly_name, __env_or("MTP_FRIENDLY_NAME",
			__env_or("MTP_MODEL", MODEL)), sizeof(g_friendly_name));
	g_device_prop_supported_cnt = 0;
	g_device_prop_supported[g_device_prop_supported_cnt++] =
		MTP_PROPERTYCODE_DEVICEFRIENDLYNAME;
	/* MTP_BATTERY_FILE: a file with the charge in percent, as power_supply capacity */
	g_battery_file = getenv("MTP_BATTERY_FILE");
	if (g_battery_file && _device_get_battery_level() >= 0)
		g_device_prop_supported[g_device_prop_supported_cnt++] =
			PTP_PROPERTYCODE_BATTERYLEVEL;
	else
		g_battery_file = NULL;
	/* MTP_DEVICE_TYPE: the perceived device type, hosts pick the icon by it (5 = PDA) */
	g_device_type = strtoul(__env_or("MTP_DEVICE_TYPE", "0"), NULL, 0);
	if (g_device_type)
		g_device_prop_supported[g_device_prop_supported_cnt++] =
			MTP_PROPERTYCODE_PERCEIVEDDEVICETYPE;
	DBG("device properties: %u\n", g_device_prop_supported_cnt);

	_prop_build_supp_props_default();
	info->ops_supported = g_ops_supported;
	info->ops_supported_cnt = (sizeof(g_ops_supported) / sizeof(mtp_uint16));
	info->events_supported = g_event_supported;
	info->events_supported_cnt = (sizeof(g_event_supported) / sizeof(mtp_uint16));
	info->device_prop_supported = g_device_prop_supported;
	info->device_prop_supported_cnt = g_device_prop_supported_cnt;
	info->capture_fmts = g_capture_fmts;
	info->capture_fmts_cnt = (sizeof(g_capture_fmts) / sizeof(mtp_uint16));
	info->object_fmts = g_object_fmts;
	info->object_fmts_cnt = (sizeof(g_object_fmts) / sizeof(mtp_uint16));
	info->std_version = MTP_STANDARD_VERSION;
	info->vendor_extn_id = MTP_VENDOR_EXTN_ID;
	info->vendor_extn_version = MTP_VENDOR_EXTN_VERSION;
	info->functional_mode = PTP_FUNCTIONMODE_STANDARD;

	info->vendor_extn_desc.num_chars = 0;
	_util_utf8_to_utf16(wtemp, sizeof(wtemp) / WCHAR_SIZ, MTP_VENDOR_EXTENSIONDESC_CHAR);
	_prop_copy_char_to_ptpstring(&(info->vendor_extn_desc), wtemp, WCHAR_TYPE);

	info->manufacturer.num_chars = 0;
	_util_utf8_to_utf16(wtemp, sizeof(wtemp) / WCHAR_SIZ, __env_or("MTP_MANUFACTURER", MTP_MANUFACTURER_CHAR));
	_prop_copy_char_to_ptpstring(&(info->manufacturer), wtemp, WCHAR_TYPE);

	info->model.num_chars = 0;
	_util_utf8_to_utf16(wtemp, sizeof(wtemp) / WCHAR_SIZ, __env_or("MTP_MODEL", MODEL));
	_prop_copy_char_to_ptpstring(&(info->model), wtemp, WCHAR_TYPE);

	info->device_version.num_chars = 0;
	_util_utf8_to_utf16(wtemp, sizeof(wtemp) / WCHAR_SIZ, __env_or("MTP_VERSION", DEVICE_VERSION));
	_prop_copy_char_to_ptpstring(&(info->device_version), wtemp, WCHAR_TYPE);

	info->serial_no.num_chars = 0;
	_util_utf8_to_utf16(wtemp, sizeof(wtemp) / WCHAR_SIZ, __env_or("MTP_SERIAL", SERIAL));
	_prop_copy_char_to_ptpstring(&(info->serial_no), wtemp, WCHAR_TYPE);
}

/* LCOV_EXCL_START */
mtp_uint32 _get_device_info_size(void)
{
	mtp_uint32 size = 0;
	device_info_t *info = &(g_device->device_info);

	size += sizeof(info->std_version);
	size += sizeof(info->vendor_extn_id);
	size += sizeof(info->vendor_extn_version);
	size += _prop_size_ptpstring(&(info->vendor_extn_desc));
	size += sizeof(info->functional_mode);

	size += sizeof(mtp_uint32);	/* will be populated by the array size of supported ops*/
	size += sizeof(g_ops_supported);

	size += sizeof(mtp_uint32);	/* will be populated by the array size of supported events*/
	size += sizeof(g_event_supported);

	size += sizeof(mtp_uint32); /* will be populated by the array size of device properties*/
	size += g_device_prop_supported_cnt * sizeof(mtp_uint16);

	size += sizeof(mtp_uint32); /* will be populated by the array size of capture formats*/
	size += sizeof(g_capture_fmts);

	size += sizeof(mtp_uint32);	/* will be populated by the array size of object formats*/
	size += sizeof(g_object_fmts);

	size += _prop_size_ptpstring(&(info->manufacturer));
	size += _prop_size_ptpstring(&(info->model));
	size += _prop_size_ptpstring(&(info->device_version));
	size += _prop_size_ptpstring(&(info->serial_no));
	return size;
}

mtp_uint32 _pack_device_info(mtp_uchar *buf, mtp_uint32 buf_sz)
{
	mtp_uint16 ii = 0;
	mtp_uchar *ptr = buf;
	mtp_uint32 count = 0;
	device_info_t *info = &(g_device->device_info);

	retv_if(NULL == buf, 0);

	retvm_if(buf_sz < _get_device_info_size(), 0,
		"buffer size [%d] is less than device_info Size\n", buf_sz);

	memcpy(ptr, &info->std_version, sizeof(info->std_version));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(info->std_version));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(info->std_version);

	memcpy(ptr, &info->vendor_extn_id, sizeof(info->vendor_extn_id));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(info->vendor_extn_id));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(info->vendor_extn_id);

	memcpy(ptr, &info->vendor_extn_version,
			sizeof(info->vendor_extn_version));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(info->vendor_extn_version));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(info->vendor_extn_version);

	ptr += _prop_pack_ptpstring(&(info->vendor_extn_desc), ptr,
			_prop_size_ptpstring(&(info->vendor_extn_desc)));

	memcpy(ptr, &info->functional_mode, sizeof(info->functional_mode));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(info->functional_mode));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(info->functional_mode);


	count = info->ops_supported_cnt;
	memcpy(ptr, &count, sizeof(count));

#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(count));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(count);
	for (ii = 0; ii < count; ii++) {
		memcpy(ptr, (void *)&(info->ops_supported[ii]),
				sizeof(mtp_uint16));
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(ptr, sizeof(mtp_uint16));
#endif /*__BIG_ENDIAN__*/
		ptr += sizeof(mtp_uint16);
	}


	count = info->events_supported_cnt;
	memcpy(ptr, &count, sizeof(count));

#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(count));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(count);
	for (ii = 0; ii < count; ii++) {
		memcpy(ptr, (void *)&(info->events_supported[ii]),
				sizeof(mtp_uint16));
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(ptr, sizeof(mtp_uint16));
#endif /*__BIG_ENDIAN__*/
		ptr += sizeof(mtp_uint16);
	}


	count = info->device_prop_supported_cnt;
	memcpy(ptr, &count, sizeof(count));

#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(count));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(count);
	for (ii = 0; ii < count; ii++) {
		memcpy(ptr, (void *)&(info->device_prop_supported[ii]),
				sizeof(mtp_uint16));
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(ptr, sizeof(mtp_uint16));
#endif /*__BIG_ENDIAN__*/
		ptr += sizeof(mtp_uint16);
	}


	count = info->capture_fmts_cnt;
	memcpy(ptr, &count, sizeof(count));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(count));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(count);

	for (ii = 0; ii < count; ii++) {
		memcpy(ptr, (void *)&(info->capture_fmts[ii]),
				sizeof(mtp_uint16));
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(ptr, sizeof(mtp_uint16));
#endif /*__BIG_ENDIAN__*/
		ptr += sizeof(mtp_uint16);
	}

	
	count = info->object_fmts_cnt;
	memcpy(ptr, &count, sizeof(count));
#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(ptr, sizeof(count));
#endif /*__BIG_ENDIAN__*/
	ptr += sizeof(count);
	for (ii = 0; ii < count; ii++) {
		memcpy(ptr, (void *)&(info->object_fmts[ii]),
				sizeof(mtp_uint16));
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(ptr, sizeof(mtp_uint16));
#endif /*__BIG_ENDIAN__*/
		ptr += sizeof(mtp_uint16);
	}


	ptr += _prop_pack_ptpstring(&(info->manufacturer), ptr,
			_prop_size_ptpstring(&(info->manufacturer)));

	ptr += _prop_pack_ptpstring(&(info->model), ptr,
			_prop_size_ptpstring(&(info->model)));

	ptr += _prop_pack_ptpstring(&(info->device_version), ptr,
			_prop_size_ptpstring(&(info->device_version)));

	ptr += _prop_pack_ptpstring(&(info->serial_no), ptr,
			_prop_size_ptpstring(&(info->serial_no)));

	return (mtp_uint32)(ptr - buf);
}

void _reset_mtp_device(void)
{
	g_status->ctrl_event_code = 0;
	g_status->mtp_op_state = MTP_STATE_ONSERVICE;

	/* resets device state to ok/Ready */
	/* reset device phase to idle */

	g_device->status = DEVICE_STATUSOK;
	g_device->phase = DEVICE_PHASE_IDLE;
}
/* LCOV_EXCL_STOP */

static mtp_bool __is_mountpoint(const mtp_char *path)
{
	struct stat st;
	struct stat parent;
	mtp_char *dir = g_path_get_dirname(path);
	mtp_bool ret = FALSE;

	if (stat(path, &st) == 0 && stat(dir, &parent) == 0)
		ret = st.st_dev != parent.st_dev;
	g_free(dir);

	return ret;
}

static mtp_bool __is_readonly(const mtp_char *path)
{
	struct statvfs st;

	return statvfs(path, &st) == 0 && (st.f_flag & ST_RDONLY);
}

/*
 * static mtp_bool _device_add_store(void)
 * This function will add the store to the device.
 * @return	TRUE if success, otherwise FALSE.
 */
static mtp_bool __add_store_to_device(void)
{
	mtp_char *storage_path = NULL;
	mtp_uint32 store_id = 0;
	file_attr_t attrs = { 0, };
	char sto_path[MTP_MAX_PATHNAME_SIZE + 1] = { 0 };

	_util_get_external_path(sto_path);
	storage_path = (mtp_char *)sto_path;
	store_id = MTP_EXTERNAL_STORE_ID;

	retvm_if(!_util_get_file_attrs(storage_path, &attrs), FALSE,
		"_util_get_file_attrs() Fail\n");

	retvm_if(MTP_FILE_ATTR_INVALID == attrs.attribute ||
		!(attrs.attribute & MTP_FILE_ATTR_MODE_DIR), FALSE,
		"attribute [0x%x], dir[0x%x]\nStorage [%d]\n",
		attrs.attribute, MTP_FILE_ATTR_MODE_DIR, store_id);

	/* the share is the card: without a card mounted there the PC gets no storage */
	retvm_if(!__is_mountpoint(storage_path), FALSE,
		"no card at [%s]\n", storage_path);

	retvm_if(g_device->num_stores + 1 > MAX_NUM_DEVICE_STORES, FALSE,
		"reached to max [%d]\n", MAX_NUM_DEVICE_STORES);

	retvm_if(!_entity_init_mtp_store(&(g_device->store_list[g_device->num_stores]),
		store_id, storage_path), FALSE,
		"_entity_init_mtp_store() Fail\n");

	if (__is_readonly(storage_path))
		g_device->store_list[g_device->num_stores].store_info.access =
			PTP_STORAGEACCESS_R;

	g_device->num_stores++;
	g_device->is_mounted[0] = TRUE;

	return TRUE;
}

/*
 * static mtp_bool _device_remove_store(void)
 * This function will remove the store from the device.
 * @return	TRUE if success, otherwise FALSE.
 */
/* LCOV_EXCL_START */
static mtp_bool __remove_store_from_device(void)
{
	mtp_store_t *store = NULL;

	store = &(g_device->store_list[0]);
	__clear_store_data(store->store_id);
	g_device->is_mounted[0] = FALSE;

	return TRUE;
}

/* LCOV_EXCL_STOP */

mtp_bool _device_install_storage(void)
{
	DBG("ADD Storage\n");
	/* LCOV_EXCL_START */
	if (g_device->is_mounted[0] == FALSE)
		__add_store_to_device();
	/* LCOV_EXCL_STOP */

	return TRUE;
}

/* LCOV_EXCL_START */
mtp_bool _device_uninstall_storage(void)
{
	if (TRUE == g_device->is_mounted[0])
		__remove_store_from_device();

	return TRUE;
}

/* LCOV_EXCL_STOP */

void _device_refresh_storage(void)
{
	mtp_char *root = NULL;

	retm_if(g_status->mtp_op_state < MTP_STATE_READY_SERVICE,
		"not serving yet\n");

	UTIL_LOCK_MUTEX(&g_cmd_inoti_mutex);

	if (g_device->is_mounted[0] == TRUE) {
		root = g_strdup(g_device->store_list[0].root_path);
		__remove_store_from_device();
#ifdef MTP_SUPPORT_OBJECTADDDELETE_EVENT
		_inoti_remove_watches(root);
#endif /*MTP_SUPPORT_OBJECTADDDELETE_EVENT*/
		g_free(root);
		_eh_send_event_req_to_eh_thread(EVENT_STORE_REMOVED,
				MTP_EXTERNAL_STORE_ID, 0, NULL);
	}

	if (__add_store_to_device())
		_eh_send_event_req_to_eh_thread(EVENT_STORE_ADDED,
				MTP_EXTERNAL_STORE_ID, 0, NULL);

	UTIL_UNLOCK_MUTEX(&g_cmd_inoti_mutex);
}

/* free space the device itself used or gave back: told to the host once the figure has settled */
void _device_report_free_space(void)
{
	static mtp_uint64 seen[MAX_NUM_DEVICE_STORES];
	static mtp_uint64 told[MAX_NUM_DEVICE_STORES];
	static mtp_bool known[MAX_NUM_DEVICE_STORES];
	fs_info_t fs_info = { 0 };
	mtp_store_t *store;
	mtp_int32 ii;

	ret_if(g_status->mtp_op_state < MTP_STATE_READY_SERVICE);
	/* a transfer holds the lock for its whole length: next time */
	ret_if(pthread_mutex_trylock(&g_cmd_inoti_mutex) != 0);

	for (ii = 0; ii < g_device->num_stores && ii < MAX_NUM_DEVICE_STORES; ii++) {
		store = &(g_device->store_list[ii]);
		if (!_util_get_filesystem_info(store->root_path, &fs_info))
			continue;

		if (!known[ii]) {
			known[ii] = TRUE;
			told[ii] = fs_info.avail_size;
		} else if (fs_info.avail_size == seen[ii] &&
				fs_info.avail_size != told[ii]) {
			told[ii] = fs_info.avail_size;
			store->store_info.free_space = fs_info.avail_size;
			_eh_send_event_req_to_eh_thread(EVENT_STORE_INFO_CHANGED,
					store->store_id, 0, NULL);
		}
		seen[ii] = fs_info.avail_size;
	}

	UTIL_UNLOCK_MUTEX(&g_cmd_inoti_mutex);
}

mtp_int32 _device_get_battery_level(void)
{
	mtp_char buf[16] = { 0 };
	mtp_int32 level;
	FILE *fp;

	retv_if(g_battery_file == NULL, -1);
	fp = fopen(g_battery_file, "r");
	retv_if(fp == NULL, -1);
	if (fgets(buf, sizeof(buf), fp) == NULL)
		buf[0] = 0;
	fclose(fp);

	retv_if(buf[0] < '0' || buf[0] > '9', -1);
	level = atoi(buf);
	return level > 100 ? 100 : level;
}

static mtp_uint32 __pack_device_type(mtp_uchar *ptr)
{
	mtp_uint32 type = g_device_type;

#ifdef __BIG_ENDIAN__
	_util_conv_byte_order(&type, sizeof(type));
#endif /* __BIG_ENDIAN__ */
	memcpy(ptr, &type, sizeof(type));
	return sizeof(type);
}

/* the value of a device property, with its description around it when desc is set; 0: not a property of this device */
mtp_uint32 _device_pack_device_prop(mtp_uint32 prop_code, mtp_bool desc,
		mtp_uchar *buf, mtp_uint32 buf_sz)
{
	ptp_string_t name = { 0 };
	mtp_uchar *ptr = buf;
	mtp_uint16 code = prop_code;
	mtp_uint16 type;
	mtp_int32 level;
	mtp_uint32 i;

	for (i = 0; i < g_device_prop_supported_cnt; i++) {
		if (g_device_prop_supported[i] == prop_code)
			break;
	}
	retv_if(i == g_device_prop_supported_cnt, 0);
	retv_if(buf_sz < 16 + 2 * sizeof(g_friendly_name), 0);

	if (prop_code == MTP_PROPERTYCODE_DEVICEFRIENDLYNAME) {
		type = PTP_DATATYPE_STRING;
		_prop_copy_char_to_ptpstring(&name, g_friendly_name, CHAR_TYPE);
	} else if (prop_code == MTP_PROPERTYCODE_PERCEIVEDDEVICETYPE) {
		type = PTP_DATATYPE_UINT32;
	} else {
		type = PTP_DATATYPE_UINT8;
		level = _device_get_battery_level();
		if (level < 0)
			level = 0;
	}

	if (desc) {
#ifdef __BIG_ENDIAN__
		_util_conv_byte_order(&code, sizeof(code));
		_util_conv_byte_order(&type, sizeof(type));
#endif /* __BIG_ENDIAN__ */
		memcpy(ptr, &code, sizeof(code));
		ptr += sizeof(code);
		memcpy(ptr, &type, sizeof(type));
		ptr += sizeof(type);
		*ptr++ = PTP_PROPGETSET_GETONLY;
		/* the factory default: no name, an empty battery, the type itself */
		if (prop_code == MTP_PROPERTYCODE_PERCEIVEDDEVICETYPE)
			ptr += __pack_device_type(ptr);
		else
			*ptr++ = 0;
	}

	if (prop_code == MTP_PROPERTYCODE_DEVICEFRIENDLYNAME) {
		ptr += _prop_pack_ptpstring(&name, ptr, _prop_size_ptpstring(&name));
		if (desc)
			*ptr++ = 0;	/* no form */
	} else if (prop_code == MTP_PROPERTYCODE_PERCEIVEDDEVICETYPE) {
		ptr += __pack_device_type(ptr);
		if (desc)
			*ptr++ = 0;	/* no form */
	} else {
		*ptr++ = (mtp_uchar)level;
		if (desc) {
			*ptr++ = 1;	/* a range: */
			*ptr++ = 0;	/* from */
			*ptr++ = 100;	/* to */
			*ptr++ = 1;	/* in steps of */
		}
	}

	return (mtp_uint32)(ptr - buf);
}

/* the battery level went up or down: the host asks for it again */
void _device_report_battery_level(void)
{
	static mtp_int32 told = -1;
	mtp_int32 level;

	ret_if(g_battery_file == NULL);
	ret_if(g_status->mtp_op_state < MTP_STATE_READY_SERVICE);

	level = _device_get_battery_level();
	ret_if(level < 0 || level == told);
	if (told >= 0)
		_eh_send_event_req_to_eh_thread(EVENT_DEVICE_PROP_CHANGED,
				PTP_PROPERTYCODE_BATTERYLEVEL, 0, NULL);
	told = level;
}

mtp_store_t *_device_get_store(mtp_uint32 store_id)
{
	mtp_int32 ii = 0;
	mtp_store_t *store = NULL;

	for (ii = 0; ii < g_device->num_stores; ii++) {

		store = &(g_device->store_list[ii]);
		if (store->store_id == store_id)
			return store;
	}
	return NULL;
}

mtp_uint32 _device_get_store_ids(ptp_array_t *store_ids)
{
	mtp_store_t *store = NULL;
	mtp_int32 ii = 0;

	for (ii = g_device->num_stores - 1; ii >= 0; ii--) {

		store = &(g_device->store_list[ii]);
		if ((store != NULL) && (FALSE == store->is_hidden))
			_prop_append_ele_ptparray(store_ids, store->store_id);
	}
	return store_ids->num_ele;
}

mtp_obj_t *_device_get_object_with_handle(mtp_uint32 obj_handle)
{
	mtp_int32 ii = 0;
	mtp_obj_t *obj = NULL;
	mtp_store_t *store = NULL;

	for (ii = 0; ii < g_device->num_stores; ii++) {

		store = &(g_device->store_list[ii]);
		obj = _entity_get_object_from_store(store, obj_handle);
		if (obj == NULL)
			continue;
		break;
	}
	return obj;
}

/* LCOV_EXCL_START */
mtp_obj_t* _device_get_object_with_path(mtp_char *full_path)
{
	mtp_int32 i = 0;
	mtp_obj_t *obj = NULL;
	mtp_store_t *store = NULL;

	retv_if(NULL == full_path, NULL);

	for (i = 0; i < g_device->num_stores; i++) {
		store = &(g_device->store_list[i]);
		obj = _entity_get_object_from_store_by_path(store, full_path);
		if (obj == NULL)
			continue;
		break;
	}
	return obj;
}
/* LCOV_EXCL_STOP */

mtp_uint16 _device_delete_object(mtp_uint32 obj_handle, mtp_uint32 fmt)
{
	mtp_uint32 ii = 0;
	mtp_uint16 response = PTP_RESPONSE_OK;
	mtp_store_t *store = NULL;
	mtp_bool all_del = TRUE;
	mtp_bool atlst_one = FALSE;

	if (PTP_OBJECTHANDLE_ALL != obj_handle) {
		store = _device_get_store_containing_obj(obj_handle);
		if (store == NULL) {
			response = PTP_RESPONSE_INVALID_OBJ_HANDLE;
		} else {
		/* LCOV_EXCL_START */
			response = _entity_delete_obj_mtp_store(store,
					obj_handle, fmt, TRUE);
		}
		return response;
	}

	for (ii = 0; ii < g_device->num_stores; ii++) {
		store = &(g_device->store_list[ii]);
		response = _entity_delete_obj_mtp_store(store, obj_handle,
				fmt, TRUE);
		switch (response) {
		case PTP_RESPONSE_STORE_READONLY:
			all_del = FALSE;
			break;
		case PTP_RESPONSE_PARTIAL_DELETION:
			all_del = FALSE;
			atlst_one = TRUE;
			break;
		case PTP_RESPONSE_OBJ_WRITEPROTECTED:
		case PTP_RESPONSE_ACCESSDENIED:
			all_del = FALSE;
			break;
		case PTP_RESPONSE_OK:
		default:
			atlst_one = TRUE;
			break;
		}
	}

	if (all_del)
		response = PTP_RESPONSE_OK;
	else if (atlst_one)
		response = PTP_RESPONSE_PARTIAL_DELETION;

	return response;
	/* LCOV_EXCL_STOP */
}

mtp_store_t *_device_get_store_containing_obj(mtp_uint32 obj_handle)
{
	mtp_uint32 ii = 0;
	mtp_obj_t *obj = NULL;
	mtp_store_t *store = NULL;

	for (ii = 0; ii < g_device->num_stores; ii++) {
		store = &(g_device->store_list[ii]);
		obj = _entity_get_object_from_store(store, obj_handle);
		if (obj != NULL)
			return store;
	}
	return NULL;
}

mtp_store_t *_device_get_store_at_index(mtp_uint32 index)
{
	retvm_if(index >= g_device->num_stores, NULL, "Index not valid\n");

	return &(g_device->store_list[index]);
}
