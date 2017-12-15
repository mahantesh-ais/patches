/*
 * Copyright 2009-2017 Citrix Ltd and other contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include "libxl_internal.h"

#define BACKEND_STRING_SIZE 5

static void disk_eject_xswatch_callback(libxl__egc *egc, libxl__ev_xswatch *w,
                                        const char *wpath, const char *epath) {
    EGC_GC;
    libxl_evgen_disk_eject *evg = (void*)w;
    const char *backend;
    char *value;
    char backend_type[BACKEND_STRING_SIZE+1];
    int rc;

    value = libxl__xs_read(gc, XBT_NULL, wpath);

    if (!value || strcmp(value,  "eject"))
        return;

    if (libxl__xs_printf(gc, XBT_NULL, wpath, "")) {
        LIBXL__EVENT_DISASTER(egc, "xs_write failed acknowledging eject",
                              errno, LIBXL_EVENT_TYPE_DISK_EJECT);
        return;
    }

    libxl_event *ev = NEW_EVENT(egc, DISK_EJECT, evg->domid, evg->user);
    libxl_device_disk *disk = &ev->u.disk_eject.disk;

    rc = libxl__xs_read_checked(gc, XBT_NULL, evg->be_ptr_path, &backend);
    if (rc) {
        LIBXL__EVENT_DISASTER(egc, "xs_read failed reading be_ptr_path",
                              errno, LIBXL_EVENT_TYPE_DISK_EJECT);
        return;
    }
    if (!backend) {
        /* device has been removed, not simply ejected */
        return;
    }

    sscanf(backend,
            "/local/domain/%d/backend/%" TOSTRING(BACKEND_STRING_SIZE)
           "[a-z]/%*d/%*d",
           &disk->backend_domid, backend_type);
    if (!strcmp(backend_type, "tap") || !strcmp(backend_type, "vbd") || !strcmp(backend_type, "vbd3")) {
        disk->backend = LIBXL_DISK_BACKEND_TAP;
    } else if (!strcmp(backend_type, "qdisk")) {
        disk->backend = LIBXL_DISK_BACKEND_QDISK;
    } else {
        disk->backend = LIBXL_DISK_BACKEND_UNKNOWN;
    }

    disk->pdev_path = strdup(""); /* xxx fixme malloc failure */
    disk->format = LIBXL_DISK_FORMAT_EMPTY;
    /* this value is returned to the user: do not free right away */
    disk->vdev = libxl__strdup(NOGC, evg->vdev);
    disk->removable = 1;
    disk->readwrite = 0;
    disk->is_cdrom = 1;

    libxl__event_occurred(egc, ev);
}

int libxl_evenable_disk_eject(libxl_ctx *ctx, uint32_t guest_domid,
                              const char *vdev, libxl_ev_user user,
                              libxl_evgen_disk_eject **evgen_out) {
    GC_INIT(ctx);
    CTX_LOCK;
    int rc;
    char *path;
    libxl_evgen_disk_eject *evg = NULL;

    evg = malloc(sizeof(*evg));  if (!evg) { rc = ERROR_NOMEM; goto out; }
    memset(evg, 0, sizeof(*evg));
    evg->user = user;
    evg->domid = guest_domid;
    LIBXL_LIST_INSERT_HEAD(&CTX->disk_eject_evgens, evg, entry);

    uint32_t domid = libxl_get_stubdom_id(ctx, guest_domid);

    if (!domid)
        domid = guest_domid;

    int devid = libxl__device_disk_dev_number(vdev, NULL, NULL);

    path = GCSPRINTF("%s/device/vbd/%d/eject",
                 libxl__xs_get_dompath(gc, domid),
                 devid);
    if (!path) { rc = ERROR_NOMEM; goto out; }

    const char *libxl_path = GCSPRINTF("%s/device/vbd/%d",
                                 libxl__xs_libxl_path(gc, domid),
                                 devid);
    evg->be_ptr_path = libxl__sprintf(NOGC, "%s/backend", libxl_path);

    const char *configured_vdev;
    rc = libxl__xs_read_checked(gc, XBT_NULL,
            GCSPRINTF("%s/dev", libxl_path), &configured_vdev);
    if (rc) goto out;

    evg->vdev = libxl__strdup(NOGC, configured_vdev);

    rc = libxl__ev_xswatch_register(gc, &evg->watch,
                                    disk_eject_xswatch_callback, path);
    if (rc) goto out;

    *evgen_out = evg;
    CTX_UNLOCK;
    GC_FREE;
    return 0;

 out:
    if (evg)
        libxl__evdisable_disk_eject(gc, evg);
    CTX_UNLOCK;
    GC_FREE;
    return rc;
}

void libxl__evdisable_disk_eject(libxl__gc *gc, libxl_evgen_disk_eject *evg) {
    CTX_LOCK;

    LIBXL_LIST_REMOVE(evg, entry);

    if (libxl__ev_xswatch_isregistered(&evg->watch))
        libxl__ev_xswatch_deregister(gc, &evg->watch);

    free(evg->vdev);
    free(evg->be_ptr_path);
    free(evg);

    CTX_UNLOCK;
}

void libxl_evdisable_disk_eject(libxl_ctx *ctx, libxl_evgen_disk_eject *evg) {
    GC_INIT(ctx);
    libxl__evdisable_disk_eject(gc, evg);
    GC_FREE;
}

int libxl__device_disk_setdefault(libxl__gc *gc, libxl_device_disk *disk,
                                  uint32_t domid)
{
    int rc;

    libxl_defbool_setdefault(&disk->discard_enable, !!disk->readwrite);
    libxl_defbool_setdefault(&disk->colo_enable, false);
    libxl_defbool_setdefault(&disk->colo_restore_enable, false);

    rc = libxl__resolve_domid(gc, disk->backend_domname, &disk->backend_domid);
    if (rc < 0) return rc;

    rc = libxl__device_disk_set_backend(gc, disk);
    return rc;
}

int libxl__device_from_disk(libxl__gc *gc, uint32_t domid,
                                   const libxl_device_disk *disk,
                                   libxl__device *device)
{
    int devid;

    devid = libxl__device_disk_dev_number(disk->vdev, NULL, NULL);
    if (devid==-1) {
        LOGD(ERROR, domid, "Invalid or unsupported"" virtual disk identifier %s",
             disk->vdev);
        return ERROR_INVAL;
    }

    device->backend_domid = disk->backend_domid;
    device->backend_devid = devid;

    switch (disk->backend) {
        case LIBXL_DISK_BACKEND_PHY:
            device->backend_kind = LIBXL__DEVICE_KIND_VBD;
            break;
        case LIBXL_DISK_BACKEND_TAP:
            device->backend_kind = LIBXL__DEVICE_KIND_VBD3;
            break;
        case LIBXL_DISK_BACKEND_QDISK:
            device->backend_kind = LIBXL__DEVICE_KIND_QDISK;
            break;
        default:
            LOGD(ERROR, domid, "Unrecognized disk backend type: %d",
                 disk->backend);
            return ERROR_INVAL;
    }

    device->domid = domid;
    device->devid = devid;
    device->kind  = LIBXL__DEVICE_KIND_VBD;

    return 0;
}

/* Specific function called directly only by local disk attach,
 * all other users should instead use the regular
 * libxl__device_disk_add wrapper
 *
 * The (optionally) passed function get_vdev will be used to
 * set the vdev the disk should be attached to. When it is set the caller
 * must also pass get_vdev_user, which will be passed to get_vdev.
 *
 * The passed get_vdev function is also in charge of printing
 * the corresponding error message when appropiate.
 */
static void device_disk_add(libxl__egc *egc, uint32_t domid,
                           libxl_device_disk *disk,
                           libxl__ao_device *aodev,
                           char *get_vdev(libxl__gc *, void *,
                                          xs_transaction_t),
                           void *get_vdev_user)
{
    STATE_AO_GC(aodev->ao);
    flexarray_t *front = NULL;
    flexarray_t *back = NULL;
    char *dev = NULL, *script;
    libxl__device *device;
    int rc;
    libxl_ctx *ctx = gc->owner;
    xs_transaction_t t = XBT_NULL;
    libxl_domain_config d_config;
    libxl_device_disk disk_saved;
    libxl__domain_userdata_lock *lock = NULL;

    libxl_domain_config_init(&d_config);
    libxl_device_disk_init(&disk_saved);
    libxl_device_disk_copy(ctx, &disk_saved, disk);

    libxl_domain_type type = libxl__domain_type(gc, domid);
    if (type == LIBXL_DOMAIN_TYPE_INVALID) {
        rc = ERROR_FAIL;
        goto out;
    }

    /*
     * get_vdev != NULL -> local attach
     * get_vdev == NULL -> block attach
     *
     * We don't care about local attach state because it's only
     * intermediate state.
     */
    if (!get_vdev && aodev->update_json) {
        lock = libxl__lock_domain_userdata(gc, domid);
        if (!lock) {
            rc = ERROR_LOCK_FAIL;
            goto out;
        }

        rc = libxl__get_domain_configuration(gc, domid, &d_config);
        if (rc) goto out;

        DEVICE_ADD(disk, disks, domid, &disk_saved, COMPARE_DISK, &d_config);

        rc = libxl__dm_check_start(gc, &d_config, domid);
        if (rc) goto out;
    }

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        if (get_vdev) {
            assert(get_vdev_user);
            disk->vdev = get_vdev(gc, get_vdev_user, t);
            if (disk->vdev == NULL) {
                rc = ERROR_FAIL;
                goto out;
            }
        }

        rc = libxl__device_disk_setdefault(gc, disk, domid);
        if (rc) goto out;

        front = flexarray_make(gc, 16, 1);
        back = flexarray_make(gc, 16, 1);

        GCNEW(device);
        rc = libxl__device_from_disk(gc, domid, disk, device);
        if (rc != 0) {
            LOGD(ERROR, domid, "Invalid or unsupported"" virtual disk identifier %s",
                 disk->vdev);
            goto out;
        }

        rc = libxl__device_exists(gc, t, device);
        if (rc < 0) goto out;
        if (rc == 1) {              /* already exists in xenstore */
            LOGD(ERROR, domid, "device already exists in xenstore");
            aodev->action = LIBXL__DEVICE_ACTION_ADD; /* for error message */
            rc = ERROR_DEVICE_EXISTS;
            goto out;
        }

        switch (disk->backend) {
            case LIBXL_DISK_BACKEND_PHY:
                dev = disk->pdev_path;

                flexarray_append(back, "params");
                flexarray_append(back, dev);

                script = libxl__abs_path(gc, disk->script?: "block",
                                         libxl__xen_script_dir_path());
                flexarray_append_pair(back, "script", script);

                assert(device->backend_kind == LIBXL__DEVICE_KIND_VBD);
                break;

            case LIBXL_DISK_BACKEND_TAP:
		rc = 0;
		dev = libxl__blktap_devpath(gc, disk->pdev_path, disk->format, disk->crypto_key_dir);
		if (!dev) {
		     LOG(ERROR, "failed to get blktap devpath for %s: %s\n",
			  disk->pdev_path, strerror(rc));
		     rc = ERROR_FAIL;
		     goto out;
		}
		LOG(DEBUG,"\nBLKTAP3_DEBUG: dev path = %s \n", dev);
		if (!disk->script && disk->backend_domid == LIBXL_TOOLSTACK_DOMID) {
		    int major, minor;
		    if (!libxl__device_physdisk_major_minor(dev, &major, &minor)) {
			LOG(DEBUG, "\nBLKTAP3_DEBUG: major:minor = %x:%x\n",major,minor);
			flexarray_append_pair(back, "physical-device",
				GCSPRINTF("%x:%x", major, minor));
		    }
		}
		flexarray_append(back, "tapdisk-params");
                flexarray_append(back, GCSPRINTF("%s:%s",
                    libxl__device_disk_string_of_format(disk->format),
                    disk->pdev_path));
		break;

            case LIBXL_DISK_BACKEND_QDISK:
                flexarray_append(back, "params");
                flexarray_append(back, GCSPRINTF("%s:%s",
                              libxl__device_disk_string_of_format(disk->format),
                              disk->pdev_path ? : ""));
                if (libxl_defbool_val(disk->colo_enable)) {
                    flexarray_append(back, "colo-host");
                    flexarray_append(back, libxl__sprintf(gc, "%s", disk->colo_host));
                    flexarray_append(back, "colo-port");
                    flexarray_append(back, libxl__sprintf(gc, "%d", disk->colo_port));
                    flexarray_append(back, "colo-export");
                    flexarray_append(back, libxl__sprintf(gc, "%s", disk->colo_export));
                    flexarray_append(back, "active-disk");
                    flexarray_append(back, libxl__sprintf(gc, "%s", disk->active_disk));
                    flexarray_append(back, "hidden-disk");
                    flexarray_append(back, libxl__sprintf(gc, "%s", disk->hidden_disk));
                }
                assert(device->backend_kind == LIBXL__DEVICE_KIND_QDISK);
                break;
            default:
                LOGD(ERROR, domid, "Unrecognized disk backend type: %d",
                     disk->backend);
                rc = ERROR_INVAL;
                goto out;
        }

        flexarray_append(back, "frontend-id");
        flexarray_append(back, GCSPRINTF("%d", domid));
        flexarray_append(back, "online");
        flexarray_append(back, "1");
        flexarray_append(back, "removable");
        flexarray_append(back, GCSPRINTF("%d", (disk->removable) ? 1 : 0));
        flexarray_append(back, "bootable");
        flexarray_append(back, GCSPRINTF("%d", 1));
        flexarray_append(back, "state");
        flexarray_append(back, GCSPRINTF("%d", XenbusStateInitialising));
        flexarray_append(back, "dev");
        flexarray_append(back, disk->vdev);
        flexarray_append(back, "type");
        flexarray_append(back, libxl__device_disk_string_of_backend(disk->backend));
        flexarray_append(back, "mode");
        flexarray_append(back, disk->readwrite ? "w" : "r");
        flexarray_append(back, "device-type");
        flexarray_append(back, disk->is_cdrom ? "cdrom" : "disk");
        if (disk->direct_io_safe) {
            flexarray_append(back, "direct-io-safe");
            flexarray_append(back, "1");
        }
        flexarray_append_pair(back, "discard-enable",
                              libxl_defbool_val(disk->discard_enable) ?
                              "1" : "0");

        flexarray_append(front, "backend-id");
        flexarray_append(front, GCSPRINTF("%d", disk->backend_domid));
        flexarray_append(front, "state");
        flexarray_append(front, GCSPRINTF("%d", XenbusStateInitialising));
        flexarray_append(front, "virtual-device");
        flexarray_append(front, GCSPRINTF("%d", device->devid));
        flexarray_append(front, "device-type");
        flexarray_append(front, disk->is_cdrom ? "cdrom" : "disk");

        /*
         * Old PV kernel disk frontends before 2.6.26 rely on tool stack to
         * write disk native protocol to frontend node. Xend does this, port
         * this behaviour to xl.
         *
         * New kernels write this node themselves. In that case it just
         * overwrites an existing node which is OK.
         */
        if (type == LIBXL_DOMAIN_TYPE_PV) {
            const char *protocol =
                xc_domain_get_native_protocol(ctx->xch, domid);
            if (protocol) {
                flexarray_append(front, "protocol");
                flexarray_append(front, libxl__strdup(gc, protocol));
            }
        }

        if (!get_vdev && aodev->update_json) {
            rc = libxl__set_domain_configuration(gc, domid, &d_config);
            if (rc) goto out;
        }

        libxl__device_generic_add(gc, t, device,
                                  libxl__xs_kvs_of_flexarray(gc, back),
                                  libxl__xs_kvs_of_flexarray(gc, front),
                                  NULL);

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

    aodev->dev = device;
    aodev->action = LIBXL__DEVICE_ACTION_ADD;
    libxl__wait_device_connection(egc, aodev);

    libxl__xs_printf(gc, XBT_NULL,
                     GCSPRINTF("%s/hotplug-status",
                               libxl__device_backend_path(gc, device)),
                     "connected");

    rc = 0;

out:
    libxl__xs_transaction_abort(gc, &t);
    if (lock) libxl__unlock_domain_userdata(lock);
    libxl_device_disk_dispose(&disk_saved);
    libxl_domain_config_dispose(&d_config);
    aodev->rc = rc;
    if (rc) aodev->callback(egc, aodev);
    return;
}

static void libxl__device_disk_add(libxl__egc *egc, uint32_t domid,
                                   libxl_device_disk *disk,
                                   libxl__ao_device *aodev)
{
    device_disk_add(egc, domid, disk, aodev, NULL, NULL);
}

static int libxl__device_disk_from_xenstore(libxl__gc *gc,
                                         const char *libxl_path,
                                         libxl_device_disk *disk)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    unsigned int len;
    char *tmp;
    int rc;

    libxl_device_disk_init(disk);

    const char *backend_path;
    rc = libxl__xs_read_checked(gc, XBT_NULL,
                                GCSPRINTF("%s/backend", libxl_path),
                                &backend_path);
    if (rc) goto out;

    if (!backend_path) {
        LOG(ERROR, "disk %s does not exist (no backend path", libxl_path);
        rc = ERROR_FAIL;
        goto out;
    }

    rc = libxl__backendpath_parse_domid(gc, backend_path, &disk->backend_domid);
    if (rc) {
        LOG(ERROR, "Unable to fetch device backend domid from %s", backend_path);
        goto out;
    }

    /*
     * "params" may not be present; but everything else must be.
     * colo releated entries(colo-host, colo-port, colo-export,
     * active-disk and hidden-disk) are present only if colo is
     * enabled.
     */
    tmp = xs_read(ctx->xsh, XBT_NULL,
                  GCSPRINTF("%s/params", libxl_path), &len);
    if (tmp && strchr(tmp, ':')) {
        disk->pdev_path = strdup(strchr(tmp, ':') + 1);
        free(tmp);
    } else {
        disk->pdev_path = tmp;
    }

    tmp = xs_read(ctx->xsh, XBT_NULL,
                  GCSPRINTF("%s/colo-host", libxl_path), &len);
    if (tmp) {
        libxl_defbool_set(&disk->colo_enable, true);
        disk->colo_host = tmp;

        tmp = xs_read(ctx->xsh, XBT_NULL,
                      GCSPRINTF("%s/colo-port", libxl_path), &len);
        if (!tmp) {
            LOG(ERROR, "Missing xenstore node %s/colo-port", libxl_path);
            goto cleanup;
        }
        disk->colo_port = atoi(tmp);

#define XS_READ_COLO(param, item) do {                                  \
        tmp = xs_read(ctx->xsh, XBT_NULL,                               \
                      GCSPRINTF("%s/"#param"", libxl_path), &len);         \
        if (!tmp) {                                                     \
            LOG(ERROR, "Missing xenstore node %s/"#param"", libxl_path);   \
            goto cleanup;                                               \
        }                                                               \
        disk->item = tmp;                                               \
} while (0)
        XS_READ_COLO(colo-export, colo_export);
        XS_READ_COLO(active-disk, active_disk);
        XS_READ_COLO(hidden-disk, hidden_disk);
#undef XS_READ_COLO
    } else {
        libxl_defbool_set(&disk->colo_enable, false);
    }

    tmp = libxl__xs_read(gc, XBT_NULL,
                         GCSPRINTF("%s/type", libxl_path));
    if (!tmp) {
        LOG(ERROR, "Missing xenstore node %s/type", libxl_path);
        goto cleanup;
    }
    libxl_string_to_backend(ctx, tmp, &(disk->backend));

    tmp = libxl__xs_read(gc, XBT_NULL,
                         GCSPRINTF("%s/tapdisk-params", libxl_path));
    if (tmp && strcmp(tmp, "")) {
    	disk->backend = LIBXL_DISK_BACKEND_TAP;
    }

    disk->vdev = xs_read(ctx->xsh, XBT_NULL,
                         GCSPRINTF("%s/dev", libxl_path), &len);
    if (!disk->vdev) {
        LOG(ERROR, "Missing xenstore node %s/dev", libxl_path);
        goto cleanup;
    }

    tmp = libxl__xs_read(gc, XBT_NULL, libxl__sprintf
                         (gc, "%s/removable", libxl_path));
    if (!tmp) {
        LOG(ERROR, "Missing xenstore node %s/removable", libxl_path);
        goto cleanup;
    }
    disk->removable = atoi(tmp);

    tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/mode", libxl_path));
    if (!tmp) {
        LOG(ERROR, "Missing xenstore node %s/mode", libxl_path);
        goto cleanup;
    }
    if (!strcmp(tmp, "w"))
        disk->readwrite = 1;
    else
        disk->readwrite = 0;

    tmp = libxl__xs_read(gc, XBT_NULL,
                         GCSPRINTF("%s/device-type", libxl_path));
    if (!tmp) {
        LOG(ERROR, "Missing xenstore node %s/device-type", libxl_path);
        goto cleanup;
    }
    disk->is_cdrom = !strcmp(tmp, "cdrom");

    disk->format = LIBXL_DISK_FORMAT_UNKNOWN;

    return 0;
cleanup:
    rc = ERROR_FAIL;
 out:
    libxl_device_disk_dispose(disk);
    return rc;
}

int libxl_vdev_to_device_disk(libxl_ctx *ctx, uint32_t domid,
                              const char *vdev, libxl_device_disk *disk)
{
    GC_INIT(ctx);
    char *dom_xl_path, *libxl_path;
    int devid = libxl__device_disk_dev_number(vdev, NULL, NULL);
    int rc = ERROR_FAIL;

    if (devid < 0)
        return ERROR_INVAL;

    libxl_device_disk_init(disk);

    dom_xl_path = libxl__xs_libxl_path(gc, domid);
    if (!dom_xl_path) {
        goto out;
    }
    libxl_path = GCSPRINTF("%s/device/vbd/%d", dom_xl_path, devid);

    rc = libxl__device_disk_from_xenstore(gc, libxl_path, disk);
out:
    GC_FREE;
    return rc;
}

static int libxl__append_disk_list(libxl__gc *gc,
                                           uint32_t domid,
                                           libxl_device_disk **disks,
                                           int *ndisks)
{
    char *libxl_dir_path = NULL;
    char **dir = NULL;
    unsigned int n = 0;
    libxl_device_disk *pdisk = NULL, *pdisk_end = NULL;
    int rc=0;
    int initial_disks = *ndisks;

    libxl_dir_path = GCSPRINTF("%s/device/vbd",
                        libxl__xs_libxl_path(gc, domid));
    dir = libxl__xs_directory(gc, XBT_NULL, libxl_dir_path, &n);
    if (dir && n) {
        libxl_device_disk *tmp;
        tmp = realloc(*disks, sizeof (libxl_device_disk) * (*ndisks + n));
        if (tmp == NULL)
            return ERROR_NOMEM;
        *disks = tmp;
        pdisk = *disks + initial_disks;
        pdisk_end = *disks + initial_disks + n;
        for (; pdisk < pdisk_end; pdisk++, dir++) {
            const char *p;
            p = GCSPRINTF("%s/%s", libxl_dir_path, *dir);
            if ((rc=libxl__device_disk_from_xenstore(gc, p, pdisk)))
                goto out;
            *ndisks += 1;
        }
    }
out:
    return rc;
}

libxl_device_disk *libxl_device_disk_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    libxl_device_disk *disks = NULL;
    int rc;

    *num = 0;

    rc = libxl__append_disk_list(gc, domid, &disks, num);
    if (rc) goto out_err;

    GC_FREE;
    return disks;

out_err:
    LOG(ERROR, "Unable to list disks");
    while (disks && *num) {
        (*num)--;
        libxl_device_disk_dispose(&disks[*num]);
    }
    free(disks);
    return NULL;
}

int libxl_device_disk_getinfo(libxl_ctx *ctx, uint32_t domid,
                              libxl_device_disk *disk, libxl_diskinfo *diskinfo)
{
    GC_INIT(ctx);
    char *dompath, *fe_path, *libxl_path;
    char *val;
    int rc;

    diskinfo->backend = NULL;

    dompath = libxl__xs_get_dompath(gc, domid);
    diskinfo->devid = libxl__device_disk_dev_number(disk->vdev, NULL, NULL);

    /* tap devices entries in xenstore are written as vbd devices. */
    fe_path = GCSPRINTF("%s/device/vbd/%d", dompath, diskinfo->devid);
    libxl_path = GCSPRINTF("%s/device/vbd/%d",
                           libxl__xs_libxl_path(gc, domid), diskinfo->devid);
    diskinfo->backend = xs_read(ctx->xsh, XBT_NULL,
                                GCSPRINTF("%s/backend", libxl_path), NULL);
    if (!diskinfo->backend) {
        GC_FREE;
        return ERROR_FAIL;
    }
    rc = libxl__backendpath_parse_domid(gc, diskinfo->backend,
                                        &diskinfo->backend_id);
    if (rc) goto out;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/state", fe_path));
    diskinfo->state = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/event-channel", fe_path));
    diskinfo->evtch = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/ring-ref", fe_path));
    diskinfo->rref = val ? strtoul(val, NULL, 10) : -1;
    diskinfo->frontend = xs_read(ctx->xsh, XBT_NULL,
                                 GCSPRINTF("%s/frontend", libxl_path), NULL);
    diskinfo->frontend_id = domid;

    GC_FREE;
    return 0;

 out:
    free(diskinfo->backend);
    return rc;
}

int libxl_cdrom_change(libxl_ctx *ctx, uint32_t domid, char *iso, libxl_device_disk *olddisk, char *vdev,
                       const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    int rc = -1;
    libxl__device device;
    libxl_device_disk disk_empty;
    xs_transaction_t t = XBT_NULL;
    char *devpath = NULL;
    char *be_path = NULL;
    char *fe_path = NULL;
    char *tmp = NULL;
    uint32_t stubdomid = libxl_get_stubdom_id(ctx, domid);
    libxl__domain_userdata_lock *lock = NULL;

    memset(&device, 0, sizeof(libxl__device));
    memset(&disk_empty, 0, sizeof(libxl_device_disk));

    struct xs_permissions roperm[2];
    roperm[0].id = 0;
    roperm[0].perms = XS_PERM_NONE;
    roperm[1].id = stubdomid;
    roperm[1].perms = XS_PERM_READ;

    /* get our empty disk setup */
    libxl_device_disk_init(&disk_empty);
    disk_empty.format = LIBXL_DISK_FORMAT_EMPTY;
    disk_empty.vdev = libxl__strdup(NOGC, olddisk->vdev);
    disk_empty.pdev_path = libxl__strdup(NOGC, "");
    disk_empty.is_cdrom = 1;
    libxl__device_disk_setdefault(gc, &disk_empty, domid);

    lock = libxl__lock_domain_userdata(gc, domid);
    if (!lock) {
        rc = ERROR_LOCK_FAIL;
        goto out;
    }

    /* tap iso, if it's already tapped it will return the existing tap */
    devpath = libxl__blktap_devpath(gc, iso, LIBXL_DISK_FORMAT_EMPTY, "/config/platform-crypto-keys");

    rc = libxl__device_from_disk(gc, stubdomid, olddisk, &device);
    if (rc){
        fprintf(stderr, "can't create device from disk\n");
        goto out;
    }
    /* insert empty cdrom */
    libxl__qmp_insert_cdrom(gc, domid, &disk_empty);

    be_path = libxl__device_backend_path(gc, &device);
    fe_path = libxl__device_frontend_path(gc, &device);

    /* disconnect iso for now */
    do {
        t = xs_transaction_start(ctx->xsh);
        tmp = libxl__xs_read(gc, t, libxl__sprintf(gc, "%s/tapdisk-params", be_path));
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/online", be_path), "%s", "0");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/state", be_path), "%s", "5");
    } while (xs_transaction_end(ctx->xsh, t, false) == false && errno == EAGAIN);

    /* make sure we're disconnected */
    rc = libxl__wait_for_backend(gc, be_path, GCSPRINTF("%d", XenbusStateClosed));
    if (rc < 0)
        goto out;

    /* Destroy old xenbus */
    do {
        t = xs_transaction_start(ctx->xsh);
        libxl__xs_rm_checked(gc, t, be_path);
        libxl__xs_rm_checked(gc, t, fe_path);
    } while (xs_transaction_end(ctx->xsh, t, false) == false && errno == EAGAIN);

    /* cleanup old tap if it's not shared with anyone else */
    if(!tapdev_is_shared(gc, tmp))
        libxl__device_destroy_tapdisk(gc, tmp, stubdomid);

    /*write new device */
    do {
        t = xs_transaction_start(ctx->xsh);
        xs_mkdir(ctx->xsh, t, be_path);
        xs_set_permissions(ctx->xsh, t, be_path, roperm, ARRAY_SIZE(roperm));
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/params", be_path), "%s", devpath);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/type", be_path), "%s", "phy");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/physical-device", be_path), "fe:%x", libxl__get_tap_minor(gc, LIBXL_DISK_FORMAT_EMPTY, iso));
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/frontend", be_path), "%s", fe_path);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/device-type", be_path), "%s", "cdrom");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/online", be_path), "%s", "1");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/state", be_path), "%s", "1");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/removable", be_path), "%s", "1");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/mode", be_path), "%s", "1");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/frontend-id", be_path), "%u", stubdomid);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/dev", be_path), "%s", "hdc");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/tapdisk-params", be_path), "%s", libxl__sprintf(gc, "aio:%s", iso));

        roperm[0].id = stubdomid;
        roperm[1].id = 0;
        xs_mkdir(ctx->xsh, t, fe_path);
        xs_set_permissions(ctx->xsh, t, fe_path, roperm, ARRAY_SIZE(roperm));
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/state", fe_path), "%s", "1");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/backend-id", fe_path), "%s", "0");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/backend", fe_path), "%s", be_path);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/virtual-device", fe_path), "%s", vdev);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/device-type", fe_path), "%s", "cdrom");
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/backend-uuid", fe_path), "%s", "00000000-0000-0000-0000-000000000000");
     } while (xs_transaction_end(ctx->xsh, t, false) == false && errno == EAGAIN);

    rc = libxl__wait_for_backend(gc, be_path, GCSPRINTF("%d", XenbusStateConnected));
    if (rc < 0)
        goto out;

    /* tell qemu to remount /dev/xvdc */
    libxl__qmp_change_cdrom(gc, domid, olddisk);

    libxl__ao_complete(egc, ao, 0);
out:
    libxl_device_disk_dispose(&disk_empty);
    if (lock) libxl__unlock_domain_userdata(lock);
    if (rc) return AO_CREATE_FAIL(rc);
    return AO_INPROGRESS;
}

int libxl_cdrom_insert(libxl_ctx *ctx, uint32_t domid, libxl_device_disk *disk,
                       const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl_device_disk *disks = NULL, disk_empty;
    libxl__domain_userdata_lock *lock = NULL;
    libxl__device device;
    xs_transaction_t t = XBT_NULL;
    int num = 0, i;
    int rc, dm_ver;
    char *be_path = NULL;
    char *libxl_path = NULL;

    libxl_device_disk_init(&disk_empty);

    disk_empty.format = LIBXL_DISK_FORMAT_EMPTY;
    disk_empty.vdev = libxl__strdup(NOGC, disk->vdev);
    disk_empty.pdev_path = libxl__strdup(NOGC, "");
    disk_empty.is_cdrom = 1;
    libxl__device_disk_setdefault(gc, &disk_empty, domid);

    lock = libxl__lock_domain_userdata(gc, domid);
    if (!lock) {
        rc = ERROR_LOCK_FAIL;
        goto out;
    }

    libxl_domain_type type = libxl__domain_type(gc, domid);
    if (type == LIBXL_DOMAIN_TYPE_INVALID) {
        rc = ERROR_FAIL;
        goto out;
    }

    if (type != LIBXL_DOMAIN_TYPE_HVM) {
        LOGD(ERROR, domid, "cdrom-insert requires an HVM domain");
        rc = ERROR_INVAL;
        goto out;
    }

    if (libxl_get_stubdom_id(ctx, domid) != 0) {
        LOGD(ERROR, domid, "cdrom-insert doesn't work for stub domains");
        rc = ERROR_INVAL;
        goto out;
    }

    dm_ver = libxl__device_model_version_running(gc, domid);
    if (dm_ver == -1) {
        LOGD(ERROR, domid, "Cannot determine device model version");
        rc = ERROR_FAIL;
        goto out;
    }

    if (dm_ver == LIBXL_DEVICE_MODEL_VERSION_NONE) {
        LOGD(ERROR, domid, "Guests without a device model cannot use cd-insert");
        rc = ERROR_FAIL;
        goto out;
    }

    disks = libxl_device_disk_list(ctx, domid, &num);
    for (i = 0; i < num; i++) {
        if (disks[i].is_cdrom && !strcmp(disk->vdev, disks[i].vdev))
        {
            /* Found.  Set backend type appropriately. */
            disk->backend=disks[i].backend;
            break;
        }
    }
    if (i == num) {
        LOGD(ERROR, domid, "Virtual device not found");
        rc = ERROR_FAIL;
        goto out;
    }

    if (!disk->pdev_path) {
        disk->pdev_path = libxl__strdup(NOGC, "");
        disk->format = LIBXL_DISK_FORMAT_EMPTY;
    }

    /* We need to eject the original image first. This is implemented
     * by inserting empty media. JSON is not updated.
     */
    if (dm_ver == LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN) {
        rc = libxl__qmp_insert_cdrom(gc, domid, &disk_empty);
        if (rc) goto out;
    }

    rc = libxl__device_from_disk(gc, domid, disk, &device);
    if (rc){
        fprintf(stderr, "can't create device from disk\n");
        goto out;
    }

    be_path = libxl__device_backend_path(gc, &device);
    libxl_path = libxl__device_libxl_path(gc, &device);

    /* Update the xenstore with new disk params */
    do {
        t = xs_transaction_start(ctx->xsh);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/params", be_path), "%s", disk->pdev_path);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/physical-device-path", be_path), "%s", disk->pdev_path);
        libxl__xs_printf(gc, t, libxl__sprintf(gc, "%s/params", libxl_path), "%s", disk->pdev_path);
     } while (xs_transaction_end(ctx->xsh, t, false) == false && errno == EAGAIN);

    /* We need to change the disk now. This is implemented
     * by inserting a non-empty disk.
     */
    if (dm_ver == LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN) {
        rc = libxl__qmp_insert_cdrom(gc, domid, disk);
        if (rc) goto out;
    }

    /* success, no actual async */
    libxl__ao_complete(egc, ao, 0);

    rc = 0;

out:
    for (i = 0; i < num; i++)
        libxl_device_disk_dispose(&disks[i]);
    free(disks);
    if (lock) libxl__unlock_domain_userdata(lock);
    libxl_device_disk_dispose(&disk_empty);
    if (rc) return AO_CREATE_FAIL(rc);
    return AO_INPROGRESS;
}

/* libxl__alloc_vdev only works on the local domain, that is the domain
 * where the toolstack is running */
static char * libxl__alloc_vdev(libxl__gc *gc, void *get_vdev_user,
        xs_transaction_t t)
{
    const char *blkdev_start = (const char *) get_vdev_user;
    int devid = 0, disk = 0, part = 0;
    char *libxl_dom_path = libxl__xs_libxl_path(gc, LIBXL_TOOLSTACK_DOMID);

    libxl__device_disk_dev_number(blkdev_start, &disk, &part);
    if (part != 0) {
        LOG(ERROR, "blkdev_start is invalid");
        return NULL;
    }

    do {
        devid = libxl__device_disk_dev_number(GCSPRINTF("d%dp0", disk),
                NULL, NULL);
        if (devid < 0)
            return NULL;
        if (libxl__xs_read(gc, t,
                    GCSPRINTF("%s/device/vbd/%d/backend",
                        libxl_dom_path, devid)) == NULL) {
            if (errno == ENOENT)
                return libxl__devid_to_vdev(gc, devid);
            else
                return NULL;
        }
        disk++;
    } while (1);
    return NULL;
}

/* Callbacks */

char *libxl__device_disk_find_local_path(libxl__gc *gc,
                                          libxl_domid guest_domid,
                                          const libxl_device_disk *disk,
                                          bool qdisk_direct)
{
    char *path = NULL;

    /* No local paths for driver domains */
    if (disk->backend_domname != NULL) {
        LOG(DEBUG, "Non-local backend, can't access locally.\n");
        goto out;
    }

    /*
     * If this is in raw format, and we're not using a script or a
     * driver domain, we can access the target path directly.
     */
    if (disk->format == LIBXL_DISK_FORMAT_RAW
        && disk->script == NULL) {
        path = libxl__strdup(gc, disk->pdev_path);
        LOG(DEBUG, "Directly accessing local RAW disk %s", path);
        goto out;
    }

    /*
     * If we're being called for a qemu path, we can pass the target
     * string directly as well
     */
    if (qdisk_direct && disk->backend == LIBXL_DISK_BACKEND_QDISK) {
        path = libxl__strdup(gc, disk->pdev_path);
        LOG(DEBUG, "Directly accessing local QDISK target %s", path);
        goto out;
    }

    /*
     * If the format isn't raw and / or we're using a script, then see
     * if the script has written a path to the "cooked" node
     */
    if (disk->script && guest_domid != INVALID_DOMID) {
        libxl__device device;
        char *be_path, *pdpath;
        int rc;

        LOGD(DEBUG, guest_domid,
             "Run from a script; checking for physical-device-path (vdev %s)",
             disk->vdev);

        rc = libxl__device_from_disk(gc, guest_domid, disk, &device);
        if (rc < 0)
            goto out;

        be_path = libxl__device_backend_path(gc, &device);

        pdpath = libxl__sprintf(gc, "%s/physical-device-path", be_path);

        LOGD(DEBUG, guest_domid, "Attempting to read node %s", pdpath);
        path = libxl__xs_read(gc, XBT_NULL, pdpath);

        if (path)
            LOGD(DEBUG, guest_domid, "Accessing cooked block device %s", path);
        else
            LOGD(DEBUG, guest_domid, "No physical-device-path, can't access locally.");

        goto out;
    }

 out:
    return path;
}

static void local_device_attach_cb(libxl__egc *egc, libxl__ao_device *aodev);

void libxl__device_disk_local_initiate_attach(libxl__egc *egc,
                                     libxl__disk_local_state *dls)
{
    STATE_AO_GC(dls->ao);
    int rc;
    const libxl_device_disk *in_disk = dls->in_disk;
    libxl_device_disk *disk = &dls->disk;
    const char *blkdev_start = dls->blkdev_start;

    assert(in_disk->pdev_path);

    disk->vdev = NULL;

    if (dls->diskpath)
        LOG(DEBUG, "Strange, dls->diskpath already set: %s", dls->diskpath);

    LOG(DEBUG, "Trying to find local path");

    dls->diskpath = libxl__device_disk_find_local_path(gc, INVALID_DOMID,
                                                       in_disk, false);
    if (dls->diskpath) {
        LOG(DEBUG, "Local path found, executing callback.");
        dls->callback(egc, dls, 0);
    } else {
        LOG(DEBUG, "Local path not found, initiating attach.");

        memcpy(disk, in_disk, sizeof(libxl_device_disk));
        disk->pdev_path = libxl__strdup(gc, in_disk->pdev_path);
        if (in_disk->script != NULL)
            disk->script = libxl__strdup(gc, in_disk->script);
        disk->vdev = NULL;
	if (in_disk->crypto_key_dir != NULL)
            disk->crypto_key_dir = libxl__strdup(gc, in_disk->crypto_key_dir);

        rc = libxl__device_disk_setdefault(gc, disk, LIBXL_TOOLSTACK_DOMID);
        if (rc) goto out;

        libxl__prepare_ao_device(ao, &dls->aodev);
        dls->aodev.callback = local_device_attach_cb;
        device_disk_add(egc, LIBXL_TOOLSTACK_DOMID, disk, &dls->aodev,
                        libxl__alloc_vdev, (void *) blkdev_start);
    }

    return;

 out:
    assert(rc);
    dls->rc = rc;
    libxl__device_disk_local_initiate_detach(egc, dls);
    dls->callback(egc, dls, rc);
}

static void local_device_attach_cb(libxl__egc *egc, libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    libxl__disk_local_state *dls = CONTAINER_OF(aodev, *dls, aodev);
    char *be_path = NULL;
    int rc;
    libxl__device device;
    libxl_device_disk *disk = &dls->disk;

    rc = aodev->rc;
    if (rc) {
        LOGE(ERROR, "unable locally attach device: %s", disk->pdev_path);
        goto out;
    }

    rc = libxl__device_from_disk(gc, LIBXL_TOOLSTACK_DOMID, disk, &device);
    if (rc < 0)
        goto out;
    be_path = libxl__device_backend_path(gc, &device);
    rc = libxl__wait_for_backend(gc, be_path, GCSPRINTF("%d", XenbusStateConnected));
    if (rc < 0)
        goto out;

    dls->diskpath = GCSPRINTF("/dev/%s",
                              libxl__devid_to_localdev(gc, device.devid));
    LOG(DEBUG, "locally attached disk %s", dls->diskpath);

    dls->callback(egc, dls, 0);
    return;

 out:
    assert(rc);
    dls->rc = rc;
    libxl__device_disk_local_initiate_detach(egc, dls);
    return;
}

/* Callbacks for local detach */

static void local_device_detach_cb(libxl__egc *egc, libxl__ao_device *aodev);

void libxl__device_disk_local_initiate_detach(libxl__egc *egc,
                                     libxl__disk_local_state *dls)
{
    STATE_AO_GC(dls->ao);
    int rc = 0;
    libxl_device_disk *disk = &dls->disk;
    libxl__device *device;
    libxl__ao_device *aodev = &dls->aodev;
    libxl__prepare_ao_device(ao, aodev);

    if (!dls->diskpath) goto out;

    if (disk->vdev != NULL) {
        GCNEW(device);
        rc = libxl__device_from_disk(gc, LIBXL_TOOLSTACK_DOMID,
                                     disk, device);
        if (rc != 0) goto out;

        aodev->action = LIBXL__DEVICE_ACTION_REMOVE;
        aodev->dev = device;
        aodev->callback = local_device_detach_cb;
        aodev->force = 0;
        libxl__initiate_device_generic_remove(egc, aodev);
        return;
    }

out:
    aodev->rc = rc;
    local_device_detach_cb(egc, aodev);
    return;
}

static void local_device_detach_cb(libxl__egc *egc, libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    libxl__disk_local_state *dls = CONTAINER_OF(aodev, *dls, aodev);
    int rc;

    if (aodev->rc) {
        LOGED(ERROR, aodev->dev->domid, "Unable to %s %s with id %u",
                     libxl__device_action_to_string(aodev->action),
                     libxl__device_kind_to_string(aodev->dev->kind),
                     aodev->dev->devid);
        goto out;
    }

out:
    /*
     * If there was an error in dls->rc, it means we have been called from
     * a failed execution of libxl__device_disk_local_initiate_attach,
     * so return the original error.
     */
    rc = dls->rc ? dls->rc : aodev->rc;
    dls->callback(egc, dls, rc);
    return;
}

/* The following functions are defined:
 * libxl_device_disk_add
 * libxl__add_disks
 * libxl_device_disk_remove
 * libxl_device_disk_destroy
 */
LIBXL_DEFINE_DEVICE_ADD(disk)
LIBXL_DEFINE_DEVICES_ADD(disk)
LIBXL_DEFINE_DEVICE_REMOVE(disk)

static int libxl_device_disk_compare(libxl_device_disk *d1,
                                     libxl_device_disk *d2)
{
    return COMPARE_DISK(d1, d2);
}

/* Take care of removable device. We maintain invariant in the
 * insert / remove operation so that:
 * 1. if xenstore is "empty" while JSON is not, the result
 *    is "empty"
 * 2. if xenstore has a different media than JSON, use the
 *    one in JSON
 * 3. if xenstore and JSON have the same media, well, you
 *    know the answer :-)
 *
 * Currently there is only one removable device -- CDROM.
 * Look for libxl_cdrom_insert for reference.
 */
static void libxl_device_disk_merge(libxl_ctx *ctx, void *d1, void *d2)
{
    GC_INIT(ctx);
    libxl_device_disk *src = d1;
    libxl_device_disk *dst = d2;

    if (src->removable) {
        if (!src->pdev_path || *src->pdev_path == '\0') {
            /* 1, no media in drive */
            free(dst->pdev_path);
            dst->pdev_path = libxl__strdup(NOGC, "");
            dst->format = LIBXL_DISK_FORMAT_EMPTY;
        } else {
            /* 2 and 3, use JSON, no need to touch anything */
            ;
        }
    }
}

static int libxl_device_disk_dm_needed(void *e, unsigned domid)
{
    libxl_device_disk *elem = e;

    return elem->backend == LIBXL_DISK_BACKEND_QDISK &&
           elem->backend_domid == domid;
}

DEFINE_DEVICE_TYPE_STRUCT(disk,
    .merge       = libxl_device_disk_merge,
    .dm_needed   = libxl_device_disk_dm_needed,
    .skip_attach = 1
);

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
