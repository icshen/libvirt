/*
 * qemu_domain.c: QEMU domain private state
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "qemu_domain.h"
#include "qemu_alias.h"
#include "qemu_block.h"
#include "qemu_cgroup.h"
#include "qemu_command.h"
#include "qemu_process.h"
#include "qemu_parse_command.h"
#include "qemu_capabilities.h"
#include "qemu_migration.h"
#include "qemu_migration_params.h"
#include "qemu_security.h"
#include "viralloc.h"
#include "virlog.h"
#include "virerror.h"
#include "c-ctype.h"
#include "cpu/cpu.h"
#include "viruuid.h"
#include "virfile.h"
#include "domain_addr.h"
#include "domain_event.h"
#include "virtime.h"
#include "virnetdevopenvswitch.h"
#include "virstoragefile.h"
#include "virstring.h"
#include "virthreadjob.h"
#include "viratomic.h"
#include "virprocess.h"
#include "vircrypto.h"
#include "virsystemd.h"
#include "secret_util.h"
#include "logging/log_manager.h"
#include "locking/domain_lock.h"

#ifdef MAJOR_IN_MKDEV
# include <sys/mkdev.h>
#elif MAJOR_IN_SYSMACROS
# include <sys/sysmacros.h>
#endif
#include <sys/time.h>
#include <fcntl.h>
#if defined(HAVE_SYS_MOUNT_H)
# include <sys/mount.h>
#endif
#ifdef WITH_SELINUX
# include <selinux/selinux.h>
#endif

#include <libxml/xpathInternals.h>
#include "dosname.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_domain");

#define QEMU_NAMESPACE_HREF "http://libvirt.org/schemas/domain/qemu/1.0"

VIR_ENUM_IMPL(qemuDomainJob, QEMU_JOB_LAST,
              "none",
              "query",
              "destroy",
              "suspend",
              "modify",
              "abort",
              "migration operation",
              "none",   /* async job is never stored in job.active */
              "async nested",
);

VIR_ENUM_IMPL(qemuDomainAsyncJob, QEMU_ASYNC_JOB_LAST,
              "none",
              "migration out",
              "migration in",
              "save",
              "dump",
              "snapshot",
              "start",
);

VIR_ENUM_IMPL(qemuDomainNamespace, QEMU_DOMAIN_NS_LAST,
              "mount",
);


#define PROC_MOUNTS "/proc/mounts"
#define DEVPREFIX "/dev/"
#define DEV_VFIO "/dev/vfio/vfio"


struct _qemuDomainLogContext {
    virObject parent;

    int writefd;
    int readfd; /* Only used if manager == NULL */
    off_t pos;
    ino_t inode; /* Only used if manager != NULL */
    char *path;
    virLogManagerPtr manager;
};

static virClassPtr qemuDomainLogContextClass;
static virClassPtr qemuDomainSaveCookieClass;

static void qemuDomainLogContextDispose(void *obj);
static void qemuDomainSaveCookieDispose(void *obj);

static int
qemuDomainOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuDomainLogContext, virClassForObject()))
        return -1;

    if (!VIR_CLASS_NEW(qemuDomainSaveCookie, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuDomain)

static void
qemuDomainLogContextDispose(void *obj)
{
    qemuDomainLogContextPtr ctxt = obj;
    VIR_DEBUG("ctxt=%p", ctxt);

    virLogManagerFree(ctxt->manager);
    VIR_FREE(ctxt->path);
    VIR_FORCE_CLOSE(ctxt->writefd);
    VIR_FORCE_CLOSE(ctxt->readfd);
}

const char *
qemuDomainAsyncJobPhaseToString(qemuDomainAsyncJob job,
                                int phase ATTRIBUTE_UNUSED)
{
    switch (job) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
    case QEMU_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeToString(phase);

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
    case QEMU_ASYNC_JOB_START:
    case QEMU_ASYNC_JOB_NONE:
        ATTRIBUTE_FALLTHROUGH;
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    return "none";
}

int
qemuDomainAsyncJobPhaseFromString(qemuDomainAsyncJob job,
                                  const char *phase)
{
    if (!phase)
        return 0;

    switch (job) {
    case QEMU_ASYNC_JOB_MIGRATION_OUT:
    case QEMU_ASYNC_JOB_MIGRATION_IN:
        return qemuMigrationJobPhaseTypeFromString(phase);

    case QEMU_ASYNC_JOB_SAVE:
    case QEMU_ASYNC_JOB_DUMP:
    case QEMU_ASYNC_JOB_SNAPSHOT:
    case QEMU_ASYNC_JOB_START:
    case QEMU_ASYNC_JOB_NONE:
        ATTRIBUTE_FALLTHROUGH;
    case QEMU_ASYNC_JOB_LAST:
        break;
    }

    if (STREQ(phase, "none"))
        return 0;
    else
        return -1;
}


bool
qemuDomainNamespaceEnabled(virDomainObjPtr vm,
                           qemuDomainNamespace ns)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    return priv->namespaces &&
        virBitmapIsBitSet(priv->namespaces, ns);
}


static int
qemuDomainEnableNamespace(virDomainObjPtr vm,
                          qemuDomainNamespace ns)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!priv->namespaces &&
        !(priv->namespaces = virBitmapNew(QEMU_DOMAIN_NS_LAST)))
        return -1;

    if (virBitmapSetBit(priv->namespaces, ns) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to enable namespace: %s"),
                       qemuDomainNamespaceTypeToString(ns));
        return -1;
    }

    return 0;
}


static void
qemuDomainDisableNamespace(virDomainObjPtr vm,
                           qemuDomainNamespace ns)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (priv->namespaces) {
        ignore_value(virBitmapClearBit(priv->namespaces, ns));
        if (virBitmapIsAllClear(priv->namespaces)) {
            virBitmapFree(priv->namespaces);
            priv->namespaces = NULL;
        }
    }
}


void qemuDomainEventQueue(virQEMUDriverPtr driver,
                          virObjectEventPtr event)
{
    if (event)
        virObjectEventStateQueue(driver->domainEventState, event);
}


void
qemuDomainEventEmitJobCompleted(virQEMUDriverPtr driver,
                                virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event;
    virTypedParameterPtr params = NULL;
    int nparams = 0;
    int type;

    if (!priv->job.completed)
        return;

    if (qemuDomainJobInfoToParams(priv->job.completed, &type,
                                  &params, &nparams) < 0) {
        VIR_WARN("Could not get stats for completed job; domain %s",
                 vm->def->name);
    }

    event = virDomainEventJobCompletedNewFromObj(vm, params, nparams);
    qemuDomainEventQueue(driver, event);
}


static int
qemuDomainObjInitJob(qemuDomainObjPrivatePtr priv)
{
    memset(&priv->job, 0, sizeof(priv->job));

    if (virCondInit(&priv->job.cond) < 0)
        return -1;

    if (virCondInit(&priv->job.asyncCond) < 0) {
        virCondDestroy(&priv->job.cond);
        return -1;
    }

    return 0;
}

static void
qemuDomainObjResetJob(qemuDomainObjPrivatePtr priv)
{
    qemuDomainJobObjPtr job = &priv->job;

    job->active = QEMU_JOB_NONE;
    job->owner = 0;
    job->ownerAPI = NULL;
    job->started = 0;
}

static void
qemuDomainObjResetAsyncJob(qemuDomainObjPrivatePtr priv)
{
    qemuDomainJobObjPtr job = &priv->job;

    job->asyncJob = QEMU_ASYNC_JOB_NONE;
    job->asyncOwner = 0;
    job->asyncOwnerAPI = NULL;
    job->asyncStarted = 0;
    job->phase = 0;
    job->mask = QEMU_JOB_DEFAULT_MASK;
    job->abortJob = false;
    job->spiceMigration = false;
    job->spiceMigrated = false;
    job->dumpCompleted = false;
    VIR_FREE(job->error);
    VIR_FREE(job->current);
    qemuMigrationParamsFree(job->migParams);
    job->migParams = NULL;
    job->apiFlags = 0;
}

void
qemuDomainObjRestoreJob(virDomainObjPtr obj,
                        qemuDomainJobObjPtr job)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    memset(job, 0, sizeof(*job));
    job->active = priv->job.active;
    job->owner = priv->job.owner;
    job->asyncJob = priv->job.asyncJob;
    job->asyncOwner = priv->job.asyncOwner;
    job->phase = priv->job.phase;
    VIR_STEAL_PTR(job->migParams, priv->job.migParams);
    job->apiFlags = priv->job.apiFlags;

    qemuDomainObjResetJob(priv);
    qemuDomainObjResetAsyncJob(priv);
}

static void
qemuDomainObjFreeJob(qemuDomainObjPrivatePtr priv)
{
    VIR_FREE(priv->job.current);
    VIR_FREE(priv->job.completed);
    virCondDestroy(&priv->job.cond);
    virCondDestroy(&priv->job.asyncCond);
}

static bool
qemuDomainTrackJob(qemuDomainJob job)
{
    return (QEMU_DOMAIN_TRACK_JOBS & JOB_MASK(job)) != 0;
}


int
qemuDomainJobInfoUpdateTime(qemuDomainJobInfoPtr jobInfo)
{
    unsigned long long now;

    if (!jobInfo->started)
        return 0;

    if (virTimeMillisNow(&now) < 0)
        return -1;

    if (now < jobInfo->started) {
        VIR_WARN("Async job starts in the future");
        jobInfo->started = 0;
        return 0;
    }

    jobInfo->timeElapsed = now - jobInfo->started;
    return 0;
}

int
qemuDomainJobInfoUpdateDowntime(qemuDomainJobInfoPtr jobInfo)
{
    unsigned long long now;

    if (!jobInfo->stopped)
        return 0;

    if (virTimeMillisNow(&now) < 0)
        return -1;

    if (now < jobInfo->stopped) {
        VIR_WARN("Guest's CPUs stopped in the future");
        jobInfo->stopped = 0;
        return 0;
    }

    jobInfo->stats.mig.downtime = now - jobInfo->stopped;
    jobInfo->stats.mig.downtime_set = true;
    return 0;
}

static virDomainJobType
qemuDomainJobStatusToType(qemuDomainJobStatus status)
{
    switch (status) {
    case QEMU_DOMAIN_JOB_STATUS_NONE:
        break;

    case QEMU_DOMAIN_JOB_STATUS_ACTIVE:
    case QEMU_DOMAIN_JOB_STATUS_MIGRATING:
    case QEMU_DOMAIN_JOB_STATUS_QEMU_COMPLETED:
    case QEMU_DOMAIN_JOB_STATUS_POSTCOPY:
    case QEMU_DOMAIN_JOB_STATUS_PAUSED:
        return VIR_DOMAIN_JOB_UNBOUNDED;

    case QEMU_DOMAIN_JOB_STATUS_COMPLETED:
        return VIR_DOMAIN_JOB_COMPLETED;

    case QEMU_DOMAIN_JOB_STATUS_FAILED:
        return VIR_DOMAIN_JOB_FAILED;

    case QEMU_DOMAIN_JOB_STATUS_CANCELED:
        return VIR_DOMAIN_JOB_CANCELLED;
    }

    return VIR_DOMAIN_JOB_NONE;
}

int
qemuDomainJobInfoToInfo(qemuDomainJobInfoPtr jobInfo,
                        virDomainJobInfoPtr info)
{
    info->type = qemuDomainJobStatusToType(jobInfo->status);
    info->timeElapsed = jobInfo->timeElapsed;

    switch (jobInfo->statsType) {
    case QEMU_DOMAIN_JOB_STATS_TYPE_MIGRATION:
        info->memTotal = jobInfo->stats.mig.ram_total;
        info->memRemaining = jobInfo->stats.mig.ram_remaining;
        info->memProcessed = jobInfo->stats.mig.ram_transferred;
        info->fileTotal = jobInfo->stats.mig.disk_total +
                          jobInfo->mirrorStats.total;
        info->fileRemaining = jobInfo->stats.mig.disk_remaining +
                              (jobInfo->mirrorStats.total -
                               jobInfo->mirrorStats.transferred);
        info->fileProcessed = jobInfo->stats.mig.disk_transferred +
                              jobInfo->mirrorStats.transferred;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP:
        info->memTotal = jobInfo->stats.mig.ram_total;
        info->memRemaining = jobInfo->stats.mig.ram_remaining;
        info->memProcessed = jobInfo->stats.mig.ram_transferred;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_MEMDUMP:
        info->memTotal = jobInfo->stats.dump.total;
        info->memProcessed = jobInfo->stats.dump.completed;
        info->memRemaining = info->memTotal - info->memProcessed;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_NONE:
        break;
    }

    info->dataTotal = info->memTotal + info->fileTotal;
    info->dataRemaining = info->memRemaining + info->fileRemaining;
    info->dataProcessed = info->memProcessed + info->fileProcessed;

    return 0;
}


static int
qemuDomainMigrationJobInfoToParams(qemuDomainJobInfoPtr jobInfo,
                                   int *type,
                                   virTypedParameterPtr *params,
                                   int *nparams)
{
    qemuMonitorMigrationStats *stats = &jobInfo->stats.mig;
    qemuDomainMirrorStatsPtr mirrorStats = &jobInfo->mirrorStats;
    virTypedParameterPtr par = NULL;
    int maxpar = 0;
    int npar = 0;
    unsigned long long mirrorRemaining = mirrorStats->total -
                                         mirrorStats->transferred;

    if (virTypedParamsAddInt(&par, &npar, &maxpar,
                             VIR_DOMAIN_JOB_OPERATION,
                             jobInfo->operation) < 0)
        goto error;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_TIME_ELAPSED,
                                jobInfo->timeElapsed) < 0)
        goto error;

    if (jobInfo->timeDeltaSet &&
        jobInfo->timeElapsed > jobInfo->timeDelta &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_TIME_ELAPSED_NET,
                                jobInfo->timeElapsed - jobInfo->timeDelta) < 0)
        goto error;

    if (stats->downtime_set &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DOWNTIME,
                                stats->downtime) < 0)
        goto error;

    if (stats->downtime_set &&
        jobInfo->timeDeltaSet &&
        stats->downtime > jobInfo->timeDelta &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DOWNTIME_NET,
                                stats->downtime - jobInfo->timeDelta) < 0)
        goto error;

    if (stats->setup_time_set &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_SETUP_TIME,
                                stats->setup_time) < 0)
        goto error;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DATA_TOTAL,
                                stats->ram_total +
                                stats->disk_total +
                                mirrorStats->total) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DATA_PROCESSED,
                                stats->ram_transferred +
                                stats->disk_transferred +
                                mirrorStats->transferred) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DATA_REMAINING,
                                stats->ram_remaining +
                                stats->disk_remaining +
                                mirrorRemaining) < 0)
        goto error;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_TOTAL,
                                stats->ram_total) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_PROCESSED,
                                stats->ram_transferred) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_REMAINING,
                                stats->ram_remaining) < 0)
        goto error;

    if (stats->ram_bps &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_BPS,
                                stats->ram_bps) < 0)
        goto error;

    if (stats->ram_duplicate_set) {
        if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_MEMORY_CONSTANT,
                                    stats->ram_duplicate) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_MEMORY_NORMAL,
                                    stats->ram_normal) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_MEMORY_NORMAL_BYTES,
                                    stats->ram_normal_bytes) < 0)
            goto error;
    }

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_DIRTY_RATE,
                                stats->ram_dirty_rate) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_ITERATION,
                                stats->ram_iteration) < 0)
        goto error;

    if (stats->ram_page_size > 0 &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_PAGE_SIZE,
                                stats->ram_page_size) < 0)
        goto error;

    /* The remaining stats are disk, mirror, or migration specific
     * so if this is a SAVEDUMP, we can just skip them */
    if (jobInfo->statsType == QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP)
        goto done;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DISK_TOTAL,
                                stats->disk_total +
                                mirrorStats->total) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DISK_PROCESSED,
                                stats->disk_transferred +
                                mirrorStats->transferred) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DISK_REMAINING,
                                stats->disk_remaining +
                                mirrorRemaining) < 0)
        goto error;

    if (stats->disk_bps &&
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_DISK_BPS,
                                stats->disk_bps) < 0)
        goto error;

    if (stats->xbzrle_set) {
        if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_COMPRESSION_CACHE,
                                    stats->xbzrle_cache_size) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_COMPRESSION_BYTES,
                                    stats->xbzrle_bytes) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_COMPRESSION_PAGES,
                                    stats->xbzrle_pages) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_COMPRESSION_CACHE_MISSES,
                                    stats->xbzrle_cache_miss) < 0 ||
            virTypedParamsAddULLong(&par, &npar, &maxpar,
                                    VIR_DOMAIN_JOB_COMPRESSION_OVERFLOW,
                                    stats->xbzrle_overflow) < 0)
            goto error;
    }

    if (stats->cpu_throttle_percentage &&
        virTypedParamsAddInt(&par, &npar, &maxpar,
                             VIR_DOMAIN_JOB_AUTO_CONVERGE_THROTTLE,
                             stats->cpu_throttle_percentage) < 0)
        goto error;

 done:
    *type = qemuDomainJobStatusToType(jobInfo->status);
    *params = par;
    *nparams = npar;
    return 0;

 error:
    virTypedParamsFree(par, npar);
    return -1;
}


static int
qemuDomainDumpJobInfoToParams(qemuDomainJobInfoPtr jobInfo,
                              int *type,
                              virTypedParameterPtr *params,
                              int *nparams)
{
    qemuMonitorDumpStats *stats = &jobInfo->stats.dump;
    virTypedParameterPtr par = NULL;
    int maxpar = 0;
    int npar = 0;

    if (virTypedParamsAddInt(&par, &npar, &maxpar,
                             VIR_DOMAIN_JOB_OPERATION,
                             jobInfo->operation) < 0)
        goto error;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_TIME_ELAPSED,
                                jobInfo->timeElapsed) < 0)
        goto error;

    if (virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_TOTAL,
                                stats->total) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_PROCESSED,
                                stats->completed) < 0 ||
        virTypedParamsAddULLong(&par, &npar, &maxpar,
                                VIR_DOMAIN_JOB_MEMORY_REMAINING,
                                stats->total - stats->completed) < 0)
        goto error;

    *type = qemuDomainJobStatusToType(jobInfo->status);
    *params = par;
    *nparams = npar;
    return 0;

 error:
    virTypedParamsFree(par, npar);
    return -1;
}


int
qemuDomainJobInfoToParams(qemuDomainJobInfoPtr jobInfo,
                          int *type,
                          virTypedParameterPtr *params,
                          int *nparams)
{
    switch (jobInfo->statsType) {
    case QEMU_DOMAIN_JOB_STATS_TYPE_MIGRATION:
    case QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP:
        return qemuDomainMigrationJobInfoToParams(jobInfo, type, params, nparams);

    case QEMU_DOMAIN_JOB_STATS_TYPE_MEMDUMP:
        return qemuDomainDumpJobInfoToParams(jobInfo, type, params, nparams);

    case QEMU_DOMAIN_JOB_STATS_TYPE_NONE:
        break;
    }

    return -1;
}


/* qemuDomainGetMasterKeyFilePath:
 * @libDir: Directory path to domain lib files
 *
 * Generate a path to the domain master key file for libDir.
 * It's up to the caller to handle checking if path exists.
 *
 * Returns path to memory containing the name of the file. It is up to the
 * caller to free; otherwise, NULL on failure.
 */
char *
qemuDomainGetMasterKeyFilePath(const char *libDir)
{
    if (!libDir) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("invalid path for master key file"));
        return NULL;
    }
    return virFileBuildPath(libDir, "master-key.aes", NULL);
}


/* qemuDomainWriteMasterKeyFile:
 * @driver: qemu driver data
 * @vm: Pointer to the vm object
 *
 * Get the desired path to the masterKey file and store it in the path.
 *
 * Returns 0 on success, -1 on failure with error message indicating failure
 */
int
qemuDomainWriteMasterKeyFile(virQEMUDriverPtr driver,
                             virDomainObjPtr vm)
{
    char *path;
    int fd = -1;
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    /* Only gets filled in if we have the capability */
    if (!priv->masterKey)
        return 0;

    if (!(path = qemuDomainGetMasterKeyFilePath(priv->libDir)))
        return -1;

    if ((fd = open(path, O_WRONLY|O_TRUNC|O_CREAT, 0600)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to open domain master key file for write"));
        goto cleanup;
    }

    if (safewrite(fd, priv->masterKey, priv->masterKeyLen) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to write master key file for domain"));
        goto cleanup;
    }

    if (qemuSecurityDomainSetPathLabel(driver->securityManager,
                                       vm->def, path, false) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    VIR_FREE(path);

    return ret;
}


static void
qemuDomainMasterKeyFree(qemuDomainObjPrivatePtr priv)
{
    if (!priv->masterKey)
        return;

    VIR_DISPOSE_N(priv->masterKey, priv->masterKeyLen);
}

/* qemuDomainMasterKeyReadFile:
 * @priv: pointer to domain private object
 *
 * Expected to be called during qemuProcessReconnect once the domain
 * libDir has been generated through qemuStateInitialize calling
 * virDomainObjListLoadAllConfigs which will restore the libDir path
 * to the domain private object.
 *
 * This function will get the path to the master key file and if it
 * exists, it will read the contents of the file saving it in priv->masterKey.
 *
 * Once the file exists, the validity checks may cause failures; however,
 * if the file doesn't exist or the capability doesn't exist, we just
 * return (mostly) quietly.
 *
 * Returns 0 on success or lack of capability
 *        -1 on failure with error message indicating failure
 */
int
qemuDomainMasterKeyReadFile(qemuDomainObjPrivatePtr priv)
{
    char *path;
    int fd = -1;
    uint8_t *masterKey = NULL;
    ssize_t masterKeyLen = 0;

    /* If we don't have the capability, then do nothing. */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_SECRET))
        return 0;

    if (!(path = qemuDomainGetMasterKeyFilePath(priv->libDir)))
        return -1;

    if (!virFileExists(path)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("domain master key file doesn't exist in %s"),
                       priv->libDir);
        goto error;
    }

    if ((fd = open(path, O_RDONLY)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to open domain master key file for read"));
        goto error;
    }

    if (VIR_ALLOC_N(masterKey, 1024) < 0)
        goto error;

    if ((masterKeyLen = saferead(fd, masterKey, 1024)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to read domain master key file"));
        goto error;
    }

    if (masterKeyLen != QEMU_DOMAIN_MASTER_KEY_LEN) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid master key read, size=%zd"), masterKeyLen);
        goto error;
    }

    ignore_value(VIR_REALLOC_N_QUIET(masterKey, masterKeyLen));

    priv->masterKey = masterKey;
    priv->masterKeyLen = masterKeyLen;

    VIR_FORCE_CLOSE(fd);
    VIR_FREE(path);

    return 0;

 error:
    if (masterKeyLen > 0)
        memset(masterKey, 0, masterKeyLen);
    VIR_FREE(masterKey);

    VIR_FORCE_CLOSE(fd);
    VIR_FREE(path);

    return -1;
}


/* qemuDomainMasterKeyRemove:
 * @priv: Pointer to the domain private object
 *
 * Remove the traces of the master key, clear the heap, clear the file,
 * delete the file.
 */
void
qemuDomainMasterKeyRemove(qemuDomainObjPrivatePtr priv)
{
    char *path = NULL;

    if (!priv->masterKey)
        return;

    /* Clear the contents */
    qemuDomainMasterKeyFree(priv);

    /* Delete the master key file */
    path = qemuDomainGetMasterKeyFilePath(priv->libDir);
    unlink(path);

    VIR_FREE(path);
}


/* qemuDomainMasterKeyCreate:
 * @vm: Pointer to the domain object
 *
 * As long as the underlying qemu has the secret capability,
 * generate and store 'raw' in a file a random 32-byte key to
 * be used as a secret shared with qemu to share sensitive data.
 *
 * Returns: 0 on success, -1 w/ error message on failure
 */
int
qemuDomainMasterKeyCreate(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    /* If we don't have the capability, then do nothing. */
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_SECRET))
        return 0;

    if (!(priv->masterKey =
          virCryptoGenerateRandom(QEMU_DOMAIN_MASTER_KEY_LEN)))
        return -1;

    priv->masterKeyLen = QEMU_DOMAIN_MASTER_KEY_LEN;

    return 0;
}


static void
qemuDomainSecretPlainClear(qemuDomainSecretPlain secret)
{
    VIR_FREE(secret.username);
    VIR_DISPOSE_N(secret.secret, secret.secretlen);
}


static void
qemuDomainSecretAESClear(qemuDomainSecretAES secret)
{
    VIR_FREE(secret.username);
    VIR_FREE(secret.alias);
    VIR_FREE(secret.iv);
    VIR_FREE(secret.ciphertext);
}


void
qemuDomainSecretInfoFree(qemuDomainSecretInfoPtr *secinfo)
{
    if (!*secinfo)
        return;

    switch ((qemuDomainSecretInfoType) (*secinfo)->type) {
    case VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN:
        qemuDomainSecretPlainClear((*secinfo)->s.plain);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_AES:
        qemuDomainSecretAESClear((*secinfo)->s.aes);
        break;

    case VIR_DOMAIN_SECRET_INFO_TYPE_LAST:
        break;
    }

    VIR_FREE(*secinfo);
}


static virClassPtr qemuDomainDiskPrivateClass;
static void qemuDomainDiskPrivateDispose(void *obj);

static int
qemuDomainDiskPrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuDomainDiskPrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuDomainDiskPrivate)

static virObjectPtr
qemuDomainDiskPrivateNew(void)
{
    qemuDomainDiskPrivatePtr priv;

    if (qemuDomainDiskPrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(qemuDomainDiskPrivateClass)))
        return NULL;

    return (virObjectPtr) priv;
}

static void
qemuDomainDiskPrivateDispose(void *obj)
{
    qemuDomainDiskPrivatePtr priv = obj;

    VIR_FREE(priv->blockJobError);
}

static virClassPtr qemuDomainStorageSourcePrivateClass;
static void qemuDomainStorageSourcePrivateDispose(void *obj);

static int
qemuDomainStorageSourcePrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuDomainStorageSourcePrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuDomainStorageSourcePrivate)

virObjectPtr
qemuDomainStorageSourcePrivateNew(void)
{
    qemuDomainStorageSourcePrivatePtr priv;

    if (qemuDomainStorageSourcePrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(qemuDomainStorageSourcePrivateClass)))
        return NULL;

    return (virObjectPtr) priv;
}


static void
qemuDomainStorageSourcePrivateDispose(void *obj)
{
    qemuDomainStorageSourcePrivatePtr priv = obj;

    qemuDomainSecretInfoFree(&priv->secinfo);
    qemuDomainSecretInfoFree(&priv->encinfo);
}


static virClassPtr qemuDomainVcpuPrivateClass;
static void qemuDomainVcpuPrivateDispose(void *obj);

static int
qemuDomainVcpuPrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuDomainVcpuPrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuDomainVcpuPrivate)

static virObjectPtr
qemuDomainVcpuPrivateNew(void)
{
    qemuDomainVcpuPrivatePtr priv;

    if (qemuDomainVcpuPrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(qemuDomainVcpuPrivateClass)))
        return NULL;

    return (virObjectPtr) priv;
}


static void
qemuDomainVcpuPrivateDispose(void *obj)
{
    qemuDomainVcpuPrivatePtr priv = obj;

    VIR_FREE(priv->type);
    VIR_FREE(priv->alias);
    return;
}


static virClassPtr qemuDomainChrSourcePrivateClass;
static void qemuDomainChrSourcePrivateDispose(void *obj);

static int
qemuDomainChrSourcePrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuDomainChrSourcePrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuDomainChrSourcePrivate)

static virObjectPtr
qemuDomainChrSourcePrivateNew(void)
{
    qemuDomainChrSourcePrivatePtr priv;

    if (qemuDomainChrSourcePrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(qemuDomainChrSourcePrivateClass)))
        return NULL;

    return (virObjectPtr) priv;
}


static void
qemuDomainChrSourcePrivateDispose(void *obj)
{
    qemuDomainChrSourcePrivatePtr priv = obj;

    qemuDomainSecretInfoFree(&priv->secinfo);
}


/* qemuDomainSecretPlainSetup:
 * @secinfo: Pointer to secret info
 * @usageType: The virSecretUsageType
 * @username: username to use for authentication (may be NULL)
 * @seclookupdef: Pointer to seclookupdef data
 *
 * Taking a secinfo, fill in the plaintext information
 *
 * Returns 0 on success, -1 on failure with error message
 */
static int
qemuDomainSecretPlainSetup(qemuDomainSecretInfoPtr secinfo,
                           virSecretUsageType usageType,
                           const char *username,
                           virSecretLookupTypeDefPtr seclookupdef)
{
    virConnectPtr conn;
    int ret = -1;

    conn = virGetConnectSecret();
    if (!conn)
        return -1;

    secinfo->type = VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN;
    if (VIR_STRDUP(secinfo->s.plain.username, username) < 0)
        goto cleanup;

    ret = virSecretGetSecretString(conn, seclookupdef, usageType,
                                   &secinfo->s.plain.secret,
                                   &secinfo->s.plain.secretlen);

 cleanup:
    virObjectUnref(conn);
    return ret;
}


/* qemuDomainSecretAESSetup:
 * @priv: pointer to domain private object
 * @secinfo: Pointer to secret info
 * @srcalias: Alias of the disk/hostdev used to generate the secret alias
 * @usageType: The virSecretUsageType
 * @username: username to use for authentication (may be NULL)
 * @seclookupdef: Pointer to seclookupdef data
 * @isLuks: True/False for is for luks (alias generation)
 *
 * Taking a secinfo, fill in the AES specific information using the
 *
 * Returns 0 on success, -1 on failure with error message
 */
static int
qemuDomainSecretAESSetup(qemuDomainObjPrivatePtr priv,
                         qemuDomainSecretInfoPtr secinfo,
                         const char *srcalias,
                         virSecretUsageType usageType,
                         const char *username,
                         virSecretLookupTypeDefPtr seclookupdef,
                         bool isLuks)
{
    virConnectPtr conn;
    int ret = -1;
    uint8_t *raw_iv = NULL;
    size_t ivlen = QEMU_DOMAIN_AES_IV_LEN;
    uint8_t *secret = NULL;
    size_t secretlen = 0;
    uint8_t *ciphertext = NULL;
    size_t ciphertextlen = 0;

    conn = virGetConnectSecret();
    if (!conn)
        return -1;

    secinfo->type = VIR_DOMAIN_SECRET_INFO_TYPE_AES;
    if (VIR_STRDUP(secinfo->s.aes.username, username) < 0)
        goto cleanup;

    if (!(secinfo->s.aes.alias = qemuDomainGetSecretAESAlias(srcalias, isLuks)))
        goto cleanup;

    /* Create a random initialization vector */
    if (!(raw_iv = virCryptoGenerateRandom(ivlen)))
        goto cleanup;

    /* Encode the IV and save that since qemu will need it */
    if (!(secinfo->s.aes.iv = virStringEncodeBase64(raw_iv, ivlen)))
        goto cleanup;

    /* Grab the unencoded secret */
    if (virSecretGetSecretString(conn, seclookupdef, usageType,
                                 &secret, &secretlen) < 0)
        goto cleanup;

    if (virCryptoEncryptData(VIR_CRYPTO_CIPHER_AES256CBC,
                             priv->masterKey, QEMU_DOMAIN_MASTER_KEY_LEN,
                             raw_iv, ivlen, secret, secretlen,
                             &ciphertext, &ciphertextlen) < 0)
        goto cleanup;

    /* Clear out the secret */
    memset(secret, 0, secretlen);

    /* Now encode the ciphertext and store to be passed to qemu */
    if (!(secinfo->s.aes.ciphertext = virStringEncodeBase64(ciphertext,
                                                            ciphertextlen)))
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_DISPOSE_N(raw_iv, ivlen);
    VIR_DISPOSE_N(secret, secretlen);
    VIR_DISPOSE_N(ciphertext, ciphertextlen);
    virObjectUnref(conn);
    return ret;
}


/* qemuDomainSecretSetup:
 * @priv: pointer to domain private object
 * @secinfo: Pointer to secret info
 * @srcalias: Alias of the disk/hostdev used to generate the secret alias
 * @usageType: The virSecretUsageType
 * @username: username to use for authentication (may be NULL)
 * @seclookupdef: Pointer to seclookupdef data
 * @isLuks: True when is luks (generates different alias)
 *
 * If we have the encryption API present and can support a secret object, then
 * build the AES secret; otherwise, build the Plain secret. This is the magic
 * decision point for utilizing the AES secrets for an RBD disk. For now iSCSI
 * disks and hostdevs will not be able to utilize this mechanism.
 *
 * Returns 0 on success, -1 on failure
 */
static int
qemuDomainSecretSetup(qemuDomainObjPrivatePtr priv,
                      qemuDomainSecretInfoPtr secinfo,
                      const char *srcalias,
                      virSecretUsageType usageType,
                      const char *username,
                      virSecretLookupTypeDefPtr seclookupdef,
                      bool isLuks)
{
    bool iscsiHasPS = virQEMUCapsGet(priv->qemuCaps,
                                     QEMU_CAPS_ISCSI_PASSWORD_SECRET);

    if (virCryptoHaveCipher(VIR_CRYPTO_CIPHER_AES256CBC) &&
        virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_SECRET) &&
        (usageType == VIR_SECRET_USAGE_TYPE_CEPH ||
         (usageType == VIR_SECRET_USAGE_TYPE_ISCSI && iscsiHasPS) ||
         usageType == VIR_SECRET_USAGE_TYPE_VOLUME ||
         usageType == VIR_SECRET_USAGE_TYPE_TLS)) {
        if (qemuDomainSecretAESSetup(priv, secinfo, srcalias,
                                     usageType, username,
                                     seclookupdef, isLuks) < 0)
            return -1;
    } else {
        if (qemuDomainSecretPlainSetup(secinfo, usageType,
                                       username, seclookupdef) < 0)
            return -1;
    }
    return 0;
}


/* qemuDomainSecretInfoNew:
 * @priv: pointer to domain private object
 * @srcAlias: Alias base to use for TLS object
 * @usageType: Secret usage type
 * @username: username for plain secrets (only)
 * @looupdef: lookup def describing secret
 * @isLuks: boolean for luks lookup
 *
 * Helper function to create a secinfo to be used for secinfo consumers
 *
 * Returns @secinfo on success, NULL on failure. Caller is responsible
 * to eventually free @secinfo.
 */
static qemuDomainSecretInfoPtr
qemuDomainSecretInfoNew(qemuDomainObjPrivatePtr priv,
                        const char *srcAlias,
                        virSecretUsageType usageType,
                        const char *username,
                        virSecretLookupTypeDefPtr lookupDef,
                        bool isLuks)
{
    qemuDomainSecretInfoPtr secinfo = NULL;

    if (VIR_ALLOC(secinfo) < 0)
        return NULL;

    if (qemuDomainSecretSetup(priv, secinfo, srcAlias, usageType,
                              username, lookupDef, isLuks) < 0)
        goto error;

    if (!username && secinfo->type == VIR_DOMAIN_SECRET_INFO_TYPE_PLAIN) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("encrypted secrets are not supported"));
        goto error;
    }

    return secinfo;

 error:
    qemuDomainSecretInfoFree(&secinfo);
    return NULL;
}


/**
 * qemuDomainSecretInfoTLSNew:
 * @priv: pointer to domain private object
 * @srcAlias: Alias base to use for TLS object
 * @secretUUID: Provide a secretUUID value to look up/create the secretInfo
 *
 * Using the passed @secretUUID, generate a seclookupdef that can be used
 * to generate the returned qemuDomainSecretInfoPtr for a TLS based secret.
 *
 * Returns qemuDomainSecretInfoPtr or NULL on error.
 */
qemuDomainSecretInfoPtr
qemuDomainSecretInfoTLSNew(qemuDomainObjPrivatePtr priv,
                           const char *srcAlias,
                           const char *secretUUID)
{
    virSecretLookupTypeDef seclookupdef = {0};

    if (virUUIDParse(secretUUID, seclookupdef.u.uuid) < 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("malformed TLS secret uuid '%s' provided"),
                       secretUUID);
        return NULL;
    }
    seclookupdef.type = VIR_SECRET_LOOKUP_TYPE_UUID;

    return qemuDomainSecretInfoNew(priv, srcAlias,
                                   VIR_SECRET_USAGE_TYPE_TLS, NULL,
                                   &seclookupdef, false);
}


static void
qemuDomainSecretStorageSourceDestroy(virStorageSourcePtr src)
{
    qemuDomainStorageSourcePrivatePtr srcPriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);

    if (srcPriv && srcPriv->secinfo)
        qemuDomainSecretInfoFree(&srcPriv->secinfo);

    if (srcPriv && srcPriv->encinfo)
        qemuDomainSecretInfoFree(&srcPriv->encinfo);
}


/* qemuDomainSecretDiskDestroy:
 * @disk: Pointer to a disk definition
 *
 * Clear and destroy memory associated with the secret
 */
void
qemuDomainSecretDiskDestroy(virDomainDiskDefPtr disk)
{
    virStorageSourcePtr next;

    for (next = disk->src; virStorageSourceIsBacking(next); next = next->backingStore)
        qemuDomainSecretStorageSourceDestroy(next);
}


bool
qemuDomainSecretDiskCapable(virStorageSourcePtr src)
{
    if (!virStorageSourceIsEmpty(src) &&
        virStorageSourceGetActualType(src) == VIR_STORAGE_TYPE_NETWORK &&
        src->auth &&
        (src->protocol == VIR_STORAGE_NET_PROTOCOL_ISCSI ||
         src->protocol == VIR_STORAGE_NET_PROTOCOL_RBD))
        return true;

    return false;
}


bool
qemuDomainDiskHasEncryptionSecret(virStorageSourcePtr src)
{
    if (!virStorageSourceIsEmpty(src) && src->encryption &&
        src->encryption->format == VIR_STORAGE_ENCRYPTION_FORMAT_LUKS &&
        src->encryption->nsecrets > 0)
        return true;

    return false;
}


/**
 * qemuDomainSecretStorageSourcePrepare:
 * @priv: domain private object
 * @src: storage source struct to setup
 * @authalias: prefix of the alias for secret holding authentication data
 * @encalias: prefix of the alias for secret holding encryption password
 *
 * Prepares data necessary for encryption and authentication of @src. The two
 * alias prefixes are provided since in the backing chain authentication belongs
 * to the storage protocol data whereas encryption is relevant to the format
 * driver in qemu. The two will have different node names.
 *
 * Returns 0 on success; -1 on error while reporting an libvirt error.
 */
static int
qemuDomainSecretStorageSourcePrepare(qemuDomainObjPrivatePtr priv,
                                     virStorageSourcePtr src,
                                     const char *authalias,
                                     const char *encalias)
{
    qemuDomainStorageSourcePrivatePtr srcPriv;
    bool hasAuth = qemuDomainSecretDiskCapable(src);
    bool hasEnc = qemuDomainDiskHasEncryptionSecret(src);

    if (!hasAuth && !hasEnc)
        return 0;

    if (!(src->privateData = qemuDomainStorageSourcePrivateNew()))
        return -1;

    srcPriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);

    if (hasAuth) {
        virSecretUsageType usageType = VIR_SECRET_USAGE_TYPE_ISCSI;

        if (src->protocol == VIR_STORAGE_NET_PROTOCOL_RBD)
            usageType = VIR_SECRET_USAGE_TYPE_CEPH;

        if (!(srcPriv->secinfo =
              qemuDomainSecretInfoNew(priv, authalias,
                                      usageType, src->auth->username,
                                      &src->auth->seclookupdef, false)))
              return -1;
    }

    if (hasEnc) {
        if (!(srcPriv->encinfo =
              qemuDomainSecretInfoNew(priv, encalias,
                                      VIR_SECRET_USAGE_TYPE_VOLUME, NULL,
                                      &src->encryption->secrets[0]->seclookupdef,
                                      true)))
              return -1;
    }

    return 0;
}


/* qemuDomainSecretDiskPrepare:
 * @priv: pointer to domain private object
 * @disk: Pointer to a disk definition
 *
 * For the right disk, generate the qemuDomainSecretInfo structure.
 *
 * Returns 0 on success, -1 on failure
 */

static int
qemuDomainSecretDiskPrepare(qemuDomainObjPrivatePtr priv,
                            virDomainDiskDefPtr disk)
{
    return qemuDomainSecretStorageSourcePrepare(priv, disk->src,
                                                disk->info.alias,
                                                disk->info.alias);
}


/* qemuDomainSecretHostdevDestroy:
 * @disk: Pointer to a hostdev definition
 *
 * Clear and destroy memory associated with the secret
 */
void
qemuDomainSecretHostdevDestroy(virDomainHostdevDefPtr hostdev)
{
    qemuDomainStorageSourcePrivatePtr srcPriv;

    if (virHostdevIsSCSIDevice(hostdev)) {
        virDomainHostdevSubsysSCSIPtr scsisrc = &hostdev->source.subsys.u.scsi;
        virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;

        if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
            srcPriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(iscsisrc->src);
            if (srcPriv && srcPriv->secinfo)
                qemuDomainSecretInfoFree(&srcPriv->secinfo);
        }
    }
}


/* qemuDomainSecretHostdevPrepare:
 * @priv: pointer to domain private object
 * @hostdev: Pointer to a hostdev definition
 *
 * For the right host device, generate the qemuDomainSecretInfo structure.
 *
 * Returns 0 on success, -1 on failure
 */
int
qemuDomainSecretHostdevPrepare(qemuDomainObjPrivatePtr priv,
                               virDomainHostdevDefPtr hostdev)
{
    if (virHostdevIsSCSIDevice(hostdev)) {
        virDomainHostdevSubsysSCSIPtr scsisrc = &hostdev->source.subsys.u.scsi;
        virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;
        virStorageSourcePtr src = iscsisrc->src;
        qemuDomainStorageSourcePrivatePtr srcPriv;

        if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI &&
            src->auth) {

            if (!(src->privateData = qemuDomainStorageSourcePrivateNew()))
                return -1;

            srcPriv = QEMU_DOMAIN_STORAGE_SOURCE_PRIVATE(src);

            if (!(srcPriv->secinfo =
                  qemuDomainSecretInfoNew(priv, hostdev->info->alias,
                                          VIR_SECRET_USAGE_TYPE_ISCSI,
                                          src->auth->username,
                                          &src->auth->seclookupdef,
                                          false)))
                return -1;
        }
    }

    return 0;
}


/* qemuDomainSecretChardevDestroy:
 * @disk: Pointer to a chardev definition
 *
 * Clear and destroy memory associated with the secret
 */
void
qemuDomainSecretChardevDestroy(virDomainChrSourceDefPtr dev)
{
    qemuDomainChrSourcePrivatePtr chrSourcePriv =
        QEMU_DOMAIN_CHR_SOURCE_PRIVATE(dev);

    if (!chrSourcePriv || !chrSourcePriv->secinfo)
        return;

    qemuDomainSecretInfoFree(&chrSourcePriv->secinfo);
}


/* qemuDomainSecretChardevPrepare:
 * @cfg: Pointer to driver config object
 * @priv: pointer to domain private object
 * @chrAlias: Alias of the chr device
 * @dev: Pointer to a char source definition
 *
 * For a TCP character device, generate a qemuDomainSecretInfo to be used
 * by the command line code to generate the secret for the tls-creds to use.
 *
 * Returns 0 on success, -1 on failure
 */
int
qemuDomainSecretChardevPrepare(virQEMUDriverConfigPtr cfg,
                               qemuDomainObjPrivatePtr priv,
                               const char *chrAlias,
                               virDomainChrSourceDefPtr dev)
{
    char *charAlias = NULL;

    if (dev->type != VIR_DOMAIN_CHR_TYPE_TCP)
        return 0;

    if (dev->data.tcp.haveTLS == VIR_TRISTATE_BOOL_YES &&
        cfg->chardevTLSx509secretUUID) {
        qemuDomainChrSourcePrivatePtr chrSourcePriv =
            QEMU_DOMAIN_CHR_SOURCE_PRIVATE(dev);

        if (!(charAlias = qemuAliasChardevFromDevAlias(chrAlias)))
            return -1;

        chrSourcePriv->secinfo =
            qemuDomainSecretInfoTLSNew(priv, charAlias,
                                       cfg->chardevTLSx509secretUUID);
        VIR_FREE(charAlias);

        if (!chrSourcePriv->secinfo)
            return -1;
    }

    return 0;
}


/* qemuDomainSecretDestroy:
 * @vm: Domain object
 *
 * Once completed with the generation of the command line it is
 * expect to remove the secrets
 */
void
qemuDomainSecretDestroy(virDomainObjPtr vm)
{
    size_t i;

    for (i = 0; i < vm->def->ndisks; i++)
        qemuDomainSecretDiskDestroy(vm->def->disks[i]);

    for (i = 0; i < vm->def->nhostdevs; i++)
        qemuDomainSecretHostdevDestroy(vm->def->hostdevs[i]);

    for (i = 0; i < vm->def->nserials; i++)
        qemuDomainSecretChardevDestroy(vm->def->serials[i]->source);

    for (i = 0; i < vm->def->nparallels; i++)
        qemuDomainSecretChardevDestroy(vm->def->parallels[i]->source);

    for (i = 0; i < vm->def->nchannels; i++)
        qemuDomainSecretChardevDestroy(vm->def->channels[i]->source);

    for (i = 0; i < vm->def->nconsoles; i++)
        qemuDomainSecretChardevDestroy(vm->def->consoles[i]->source);

    for (i = 0; i < vm->def->nsmartcards; i++) {
        if (vm->def->smartcards[i]->type ==
            VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH)
            qemuDomainSecretChardevDestroy(vm->def->smartcards[i]->data.passthru);
    }

    for (i = 0; i < vm->def->nrngs; i++) {
        if (vm->def->rngs[i]->backend == VIR_DOMAIN_RNG_BACKEND_EGD)
            qemuDomainSecretChardevDestroy(vm->def->rngs[i]->source.chardev);
    }

    for (i = 0; i < vm->def->nredirdevs; i++)
        qemuDomainSecretChardevDestroy(vm->def->redirdevs[i]->source);
}


/* qemuDomainSecretPrepare:
 * @driver: Pointer to driver object
 * @vm: Domain object
 *
 * For any objects that may require an auth/secret setup, create a
 * qemuDomainSecretInfo and save it in the approriate place within
 * the private structures. This will be used by command line build
 * code in order to pass the secret along to qemu in order to provide
 * the necessary authentication data.
 *
 * Returns 0 on success, -1 on failure with error message set
 */
int
qemuDomainSecretPrepare(virQEMUDriverPtr driver,
                        virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    size_t i;
    int ret = -1;

    /* disk secrets are prepared when preparing disks */

    for (i = 0; i < vm->def->nhostdevs; i++) {
        if (qemuDomainSecretHostdevPrepare(priv,
                                           vm->def->hostdevs[i]) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nserials; i++) {
        if (qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->serials[i]->info.alias,
                                           vm->def->serials[i]->source) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nparallels; i++) {
        if (qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->parallels[i]->info.alias,
                                           vm->def->parallels[i]->source) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nchannels; i++) {
        if (qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->channels[i]->info.alias,
                                           vm->def->channels[i]->source) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nconsoles; i++) {
        if (qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->consoles[i]->info.alias,
                                           vm->def->consoles[i]->source) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nsmartcards; i++)
        if (vm->def->smartcards[i]->type ==
            VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH &&
            qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->smartcards[i]->info.alias,
                                           vm->def->smartcards[i]->data.passthru) < 0)
            goto cleanup;

    for (i = 0; i < vm->def->nrngs; i++) {
        if (vm->def->rngs[i]->backend == VIR_DOMAIN_RNG_BACKEND_EGD &&
            qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->rngs[i]->info.alias,
                                           vm->def->rngs[i]->source.chardev) < 0)
            goto cleanup;
    }

    for (i = 0; i < vm->def->nredirdevs; i++) {
        if (qemuDomainSecretChardevPrepare(cfg, priv,
                                           vm->def->redirdevs[i]->info.alias,
                                           vm->def->redirdevs[i]->source) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


/* This is the old way of setting up per-domain directories */
static int
qemuDomainSetPrivatePathsOld(virQEMUDriverPtr driver,
                             virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (!priv->libDir &&
        virAsprintf(&priv->libDir, "%s/domain-%s",
                    cfg->libDir, vm->def->name) < 0)
        goto cleanup;

    if (!priv->channelTargetDir &&
        virAsprintf(&priv->channelTargetDir, "%s/domain-%s",
                    cfg->channelTargetDir, vm->def->name) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}


int
qemuDomainSetPrivatePaths(virQEMUDriverPtr driver,
                          virDomainObjPtr vm)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char *domname = virDomainDefGetShortName(vm->def);
    int ret = -1;

    if (!domname)
        goto cleanup;

    if (!priv->libDir &&
        virAsprintf(&priv->libDir, "%s/domain-%s", cfg->libDir, domname) < 0)
        goto cleanup;

    if (!priv->channelTargetDir &&
        virAsprintf(&priv->channelTargetDir, "%s/domain-%s",
                    cfg->channelTargetDir, domname) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    VIR_FREE(domname);
    return ret;
}


static void *
qemuDomainObjPrivateAlloc(void *opaque)
{
    qemuDomainObjPrivatePtr priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    if (qemuDomainObjInitJob(priv) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to init qemu driver mutexes"));
        goto error;
    }

    if (!(priv->devs = virChrdevAlloc()))
        goto error;

    priv->migMaxBandwidth = QEMU_DOMAIN_MIG_BANDWIDTH_MAX;
    priv->driver = opaque;

    return priv;

 error:
    VIR_FREE(priv);
    return NULL;
}

/**
 * qemuDomainObjPrivateDataClear:
 * @priv: domain private data
 *
 * Clears private data entries, which are not necessary or stale if the VM is
 * not running.
 */
void
qemuDomainObjPrivateDataClear(qemuDomainObjPrivatePtr priv)
{
    virStringListFree(priv->qemuDevices);
    priv->qemuDevices = NULL;

    virCgroupFree(&priv->cgroup);

    virPerfFree(priv->perf);
    priv->perf = NULL;

    VIR_FREE(priv->machineName);

    virObjectUnref(priv->qemuCaps);
    priv->qemuCaps = NULL;

    VIR_FREE(priv->pidfile);

    VIR_FREE(priv->libDir);
    VIR_FREE(priv->channelTargetDir);

    /* remove automatic pinning data */
    virBitmapFree(priv->autoNodeset);
    priv->autoNodeset = NULL;
    virBitmapFree(priv->autoCpuset);
    priv->autoCpuset = NULL;

    /* remove address data */
    virDomainPCIAddressSetFree(priv->pciaddrs);
    priv->pciaddrs = NULL;
    virDomainUSBAddressSetFree(priv->usbaddrs);
    priv->usbaddrs = NULL;

    virCPUDefFree(priv->origCPU);
    priv->origCPU = NULL;

    /* clear previously used namespaces */
    virBitmapFree(priv->namespaces);
    priv->namespaces = NULL;

    priv->reconnectBlockjobs = VIR_TRISTATE_BOOL_ABSENT;
    priv->allowReboot = VIR_TRISTATE_BOOL_ABSENT;

    virBitmapFree(priv->migrationCaps);
    priv->migrationCaps = NULL;

    qemuDomainObjResetJob(priv);
    qemuDomainObjResetAsyncJob(priv);
}


static void
qemuDomainObjPrivateFree(void *data)
{
    qemuDomainObjPrivatePtr priv = data;

    qemuDomainObjPrivateDataClear(priv);

    virDomainChrSourceDefFree(priv->monConfig);
    qemuDomainObjFreeJob(priv);
    VIR_FREE(priv->lockState);
    VIR_FREE(priv->origname);

    virChrdevFree(priv->devs);

    /* This should never be non-NULL if we get here, but just in case... */
    if (priv->mon) {
        VIR_ERROR(_("Unexpected QEMU monitor still active during domain deletion"));
        qemuMonitorClose(priv->mon);
    }
    if (priv->agent) {
        VIR_ERROR(_("Unexpected QEMU agent still active during domain deletion"));
        qemuAgentClose(priv->agent);
    }
    VIR_FREE(priv->cleanupCallbacks);

    qemuDomainSecretInfoFree(&priv->migSecinfo);
    qemuDomainMasterKeyFree(priv);

    VIR_FREE(priv);
}


static int
qemuStorageSourcePrivateDataParse(xmlXPathContextPtr ctxt,
                                  virStorageSourcePtr src)
{
    src->nodestorage = virXPathString("string(./nodenames/nodename[@type='storage']/@name)", ctxt);
    src->nodeformat = virXPathString("string(./nodenames/nodename[@type='format']/@name)", ctxt);

    if (virStorageSourcePrivateDataParseRelPath(ctxt, src) < 0)
        return -1;

    return 0;
}


static int
qemuStorageSourcePrivateDataFormat(virStorageSourcePtr src,
                                   virBufferPtr buf)
{
    if (src->nodestorage || src->nodeformat) {
        virBufferAddLit(buf, "<nodenames>\n");
        virBufferAdjustIndent(buf, 2);
        virBufferEscapeString(buf, "<nodename type='storage' name='%s'/>\n", src->nodestorage);
        virBufferEscapeString(buf, "<nodename type='format' name='%s'/>\n", src->nodeformat);
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</nodenames>\n");
    }

    if (virStorageSourcePrivateDataFormatRelPath(src, buf) < 0)
        return -1;

    return 0;
}


static void
qemuDomainObjPrivateXMLFormatVcpus(virBufferPtr buf,
                                   virDomainDefPtr def)
{
    size_t i;
    size_t maxvcpus = virDomainDefGetVcpusMax(def);
    virDomainVcpuDefPtr vcpu;
    pid_t tid;

    virBufferAddLit(buf, "<vcpus>\n");
    virBufferAdjustIndent(buf, 2);

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);
        tid = QEMU_DOMAIN_VCPU_PRIVATE(vcpu)->tid;

        if (!vcpu->online || tid == 0)
            continue;

        virBufferAsprintf(buf, "<vcpu id='%zu' pid='%d'/>\n", i, tid);
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</vcpus>\n");
}


static int
qemuDomainObjPrivateXMLFormatAutomaticPlacement(virBufferPtr buf,
                                                qemuDomainObjPrivatePtr priv)
{
    char *nodeset = NULL;
    char *cpuset = NULL;
    int ret = -1;

    if (!priv->autoNodeset && !priv->autoCpuset)
        return 0;

    if (priv->autoNodeset &&
        !((nodeset = virBitmapFormat(priv->autoNodeset))))
        goto cleanup;

    if (priv->autoCpuset &&
        !((cpuset = virBitmapFormat(priv->autoCpuset))))
        goto cleanup;

    virBufferAddLit(buf, "<numad");
    virBufferEscapeString(buf, " nodeset='%s'", nodeset);
    virBufferEscapeString(buf, " cpuset='%s'", cpuset);
    virBufferAddLit(buf, "/>\n");

    ret = 0;

 cleanup:
    VIR_FREE(nodeset);
    VIR_FREE(cpuset);
    return ret;
}


static int
qemuDomainObjPrivateXMLFormatBlockjobs(virBufferPtr buf,
                                       virDomainObjPtr vm)
{
    virBuffer attrBuf = VIR_BUFFER_INITIALIZER;
    bool bj = qemuDomainHasBlockjob(vm, false);

    virBufferAsprintf(&attrBuf, " active='%s'",
                      virTristateBoolTypeToString(virTristateBoolFromBool(bj)));

    return virXMLFormatElement(buf, "blockjobs", &attrBuf, NULL);
}


void
qemuDomainObjPrivateXMLFormatAllowReboot(virBufferPtr buf,
                                         virTristateBool allowReboot)
{
    virBufferAsprintf(buf, "<allowReboot value='%s'/>\n",
                      virTristateBoolTypeToString(allowReboot));

}


static int
qemuDomainObjPrivateXMLFormatJob(virBufferPtr buf,
                                 virDomainObjPtr vm,
                                 qemuDomainObjPrivatePtr priv)
{
    virBuffer attrBuf = VIR_BUFFER_INITIALIZER;
    virBuffer childBuf = VIR_BUFFER_INITIALIZER;
    qemuDomainJob job = priv->job.active;

    if (!qemuDomainTrackJob(job))
        job = QEMU_JOB_NONE;

    if (job == QEMU_JOB_NONE &&
        priv->job.asyncJob == QEMU_ASYNC_JOB_NONE)
        return 0;

    virBufferSetChildIndent(&childBuf, buf);

    virBufferAsprintf(&attrBuf, " type='%s' async='%s'",
                      qemuDomainJobTypeToString(job),
                      qemuDomainAsyncJobTypeToString(priv->job.asyncJob));

    if (priv->job.phase) {
        virBufferAsprintf(&attrBuf, " phase='%s'",
                          qemuDomainAsyncJobPhaseToString(priv->job.asyncJob,
                                                          priv->job.phase));
    }

    if (priv->job.asyncJob != QEMU_ASYNC_JOB_NONE)
        virBufferAsprintf(&attrBuf, " flags='0x%lx'", priv->job.apiFlags);

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_OUT) {
        size_t i;
        virDomainDiskDefPtr disk;
        qemuDomainDiskPrivatePtr diskPriv;

        for (i = 0; i < vm->def->ndisks; i++) {
            disk = vm->def->disks[i];
            diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
            virBufferAsprintf(&childBuf, "<disk dev='%s' migrating='%s'/>\n",
                              disk->dst, diskPriv->migrating ? "yes" : "no");
        }
    }

    if (priv->job.migParams)
        qemuMigrationParamsFormat(&childBuf, priv->job.migParams);

    return virXMLFormatElement(buf, "job", &attrBuf, &childBuf);
}


static int
qemuDomainObjPrivateXMLFormat(virBufferPtr buf,
                              virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    const char *monitorpath;

    /* priv->monitor_chr is set only for qemu */
    if (priv->monConfig) {
        switch (priv->monConfig->type) {
        case VIR_DOMAIN_CHR_TYPE_UNIX:
            monitorpath = priv->monConfig->data.nix.path;
            break;
        default:
        case VIR_DOMAIN_CHR_TYPE_PTY:
            monitorpath = priv->monConfig->data.file.path;
            break;
        }

        virBufferEscapeString(buf, "<monitor path='%s'", monitorpath);
        if (priv->monJSON)
            virBufferAddLit(buf, " json='1'");
        virBufferAsprintf(buf, " type='%s'/>\n",
                          virDomainChrTypeToString(priv->monConfig->type));
    }

    if (priv->namespaces) {
        ssize_t ns = -1;

        virBufferAddLit(buf, "<namespaces>\n");
        virBufferAdjustIndent(buf, 2);
        while ((ns = virBitmapNextSetBit(priv->namespaces, ns)) >= 0)
            virBufferAsprintf(buf, "<%s/>\n", qemuDomainNamespaceTypeToString(ns));
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</namespaces>\n");
    }

    qemuDomainObjPrivateXMLFormatVcpus(buf, vm->def);

    if (priv->qemuCaps) {
        size_t i;
        virBufferAddLit(buf, "<qemuCaps>\n");
        virBufferAdjustIndent(buf, 2);
        for (i = 0; i < QEMU_CAPS_LAST; i++) {
            if (virQEMUCapsGet(priv->qemuCaps, i)) {
                virBufferAsprintf(buf, "<flag name='%s'/>\n",
                                  virQEMUCapsTypeToString(i));
            }
        }
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</qemuCaps>\n");
    }

    if (priv->lockState)
        virBufferAsprintf(buf, "<lockstate>%s</lockstate>\n", priv->lockState);

    if (qemuDomainObjPrivateXMLFormatJob(buf, vm, priv) < 0)
        return -1;

    if (priv->fakeReboot)
        virBufferAddLit(buf, "<fakereboot/>\n");

    if (priv->qemuDevices && *priv->qemuDevices) {
        char **tmp = priv->qemuDevices;
        virBufferAddLit(buf, "<devices>\n");
        virBufferAdjustIndent(buf, 2);
        while (*tmp) {
            virBufferAsprintf(buf, "<device alias='%s'/>\n", *tmp);
            tmp++;
        }
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</devices>\n");
    }

    if (qemuDomainObjPrivateXMLFormatAutomaticPlacement(buf, priv) < 0)
        return -1;

    /* Various per-domain paths */
    virBufferEscapeString(buf, "<libDir path='%s'/>\n", priv->libDir);
    virBufferEscapeString(buf, "<channelTargetDir path='%s'/>\n",
                          priv->channelTargetDir);

    virCPUDefFormatBufFull(buf, priv->origCPU, NULL);

    if (priv->chardevStdioLogd)
        virBufferAddLit(buf, "<chardevStdioLogd/>\n");

    qemuDomainObjPrivateXMLFormatAllowReboot(buf, priv->allowReboot);

    if (qemuDomainObjPrivateXMLFormatBlockjobs(buf, vm) < 0)
        return -1;

    return 0;
}


static int
qemuDomainObjPrivateXMLParseVcpu(xmlNodePtr node,
                                 unsigned int idx,
                                 virDomainDefPtr def)
{
    virDomainVcpuDefPtr vcpu;
    char *idstr;
    char *pidstr;
    unsigned int tmp;
    int ret = -1;

    idstr = virXMLPropString(node, "id");

    if (idstr &&
        (virStrToLong_uip(idstr, NULL, 10, &idx) < 0)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot parse vcpu index '%s'"), idstr);
        goto cleanup;
    }
    if (!(vcpu = virDomainDefGetVcpu(def, idx))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid vcpu index '%u'"), idx);
        goto cleanup;
    }

    if (!(pidstr = virXMLPropString(node, "pid")))
        goto cleanup;

    if (virStrToLong_uip(pidstr, NULL, 10, &tmp) < 0)
        goto cleanup;

    QEMU_DOMAIN_VCPU_PRIVATE(vcpu)->tid = tmp;

    ret = 0;

 cleanup:
    VIR_FREE(idstr);
    VIR_FREE(pidstr);
    return ret;
}


static int
qemuDomainObjPrivateXMLParseAutomaticPlacement(xmlXPathContextPtr ctxt,
                                               qemuDomainObjPrivatePtr priv,
                                               virQEMUDriverPtr driver)
{
    virCapsPtr caps = NULL;
    char *nodeset;
    char *cpuset;
    int nodesetSize = 0;
    size_t i;
    int ret = -1;

    nodeset = virXPathString("string(./numad/@nodeset)", ctxt);
    cpuset = virXPathString("string(./numad/@cpuset)", ctxt);

    if (!nodeset && !cpuset)
        return 0;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    /* Figure out how big the nodeset bitmap needs to be.
     * This is necessary because NUMA node IDs are not guaranteed to
     * start from 0 or be densely allocated */
    for (i = 0; i < caps->host.nnumaCell; i++)
        nodesetSize = MAX(nodesetSize, caps->host.numaCell[i]->num + 1);

    if (nodeset &&
        virBitmapParse(nodeset, &priv->autoNodeset, nodesetSize) < 0)
        goto cleanup;

    if (cpuset) {
        if (virBitmapParse(cpuset, &priv->autoCpuset, VIR_DOMAIN_CPUMASK_LEN) < 0)
            goto cleanup;
    } else {
        /* autoNodeset is present in this case, since otherwise we wouldn't
         * reach this code */
        if (!(priv->autoCpuset = virCapabilitiesGetCpusForNodemask(caps,
                                                                   priv->autoNodeset)))
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virObjectUnref(caps);
    VIR_FREE(nodeset);
    VIR_FREE(cpuset);

    return ret;
}


static int
qemuDomainObjPrivateXMLParseBlockjobs(qemuDomainObjPrivatePtr priv,
                                      xmlXPathContextPtr ctxt)
{
    char *active;
    int tmp;

    if ((active = virXPathString("string(./blockjobs/@active)", ctxt)) &&
        (tmp = virTristateBoolTypeFromString(active)) > 0)
        priv->reconnectBlockjobs = tmp;

    VIR_FREE(active);
    return 0;
}


int
qemuDomainObjPrivateXMLParseAllowReboot(xmlXPathContextPtr ctxt,
                                        virTristateBool *allowReboot)
{
    int ret = -1;
    int val;
    char *valStr;

    if ((valStr = virXPathString("string(./allowReboot/@value)", ctxt))) {
        if ((val = virTristateBoolTypeFromString(valStr)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid allowReboot value '%s'"), valStr);
            goto cleanup;
        }
        *allowReboot = val;
    }

    ret = 0;

 cleanup:
    VIR_FREE(valStr);
    return ret;
}


static int
qemuDomainObjPrivateXMLParseJob(virDomainObjPtr vm,
                                qemuDomainObjPrivatePtr priv,
                                xmlXPathContextPtr ctxt)
{
    xmlNodePtr *nodes = NULL;
    xmlNodePtr savedNode = ctxt->node;
    char *tmp = NULL;
    size_t i;
    int n;
    int ret = -1;

    if (!(ctxt->node = virXPathNode("./job[1]", ctxt))) {
        ret = 0;
        goto cleanup;
    }

    if ((tmp = virXPathString("string(@type)", ctxt))) {
        int type;

        if ((type = qemuDomainJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown job type %s"), tmp);
            goto cleanup;
        }
        VIR_FREE(tmp);
        priv->job.active = type;
    }

    if ((tmp = virXPathString("string(@async)", ctxt))) {
        int async;

        if ((async = qemuDomainAsyncJobTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown async job type %s"), tmp);
            goto cleanup;
        }
        VIR_FREE(tmp);
        priv->job.asyncJob = async;

        if ((tmp = virXPathString("string(@phase)", ctxt))) {
            priv->job.phase = qemuDomainAsyncJobPhaseFromString(async, tmp);
            if (priv->job.phase < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unknown job phase %s"), tmp);
                goto cleanup;
            }
            VIR_FREE(tmp);
        }
    }

    if (virXPathULongHex("string(@flags)", ctxt, &priv->job.apiFlags) == -2) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid job flags"));
        goto cleanup;
    }

    if ((n = virXPathNodeSet("./disk[@migrating='yes']", ctxt, &nodes)) < 0)
        goto cleanup;

    if (n > 0) {
        if (priv->job.asyncJob != QEMU_ASYNC_JOB_MIGRATION_OUT) {
            VIR_WARN("Found disks marked for migration but we were not "
                     "migrating");
            n = 0;
        }
        for (i = 0; i < n; i++) {
            char *dst = virXMLPropString(nodes[i], "dev");
            virDomainDiskDefPtr disk;

            if (dst && (disk = virDomainDiskByName(vm->def, dst, false)))
                QEMU_DOMAIN_DISK_PRIVATE(disk)->migrating = true;
            VIR_FREE(dst);
        }
    }
    VIR_FREE(nodes);

    if (qemuMigrationParamsParse(ctxt, &priv->job.migParams) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    ctxt->node = savedNode;
    VIR_FREE(tmp);
    VIR_FREE(nodes);
    return ret;
}


static int
qemuDomainObjPrivateXMLParse(xmlXPathContextPtr ctxt,
                             virDomainObjPtr vm,
                             virDomainDefParserConfigPtr config)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = config->priv;
    char *monitorpath;
    char *tmp = NULL;
    int n;
    size_t i;
    xmlNodePtr *nodes = NULL;
    xmlNodePtr node = NULL;
    virQEMUCapsPtr qemuCaps = NULL;

    if (!(priv->monConfig = virDomainChrSourceDefNew(NULL)))
        goto error;

    if (!(monitorpath =
          virXPathString("string(./monitor[1]/@path)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("no monitor path"));
        goto error;
    }

    tmp = virXPathString("string(./monitor[1]/@type)", ctxt);
    if (tmp)
        priv->monConfig->type = virDomainChrTypeFromString(tmp);
    else
        priv->monConfig->type = VIR_DOMAIN_CHR_TYPE_PTY;
    VIR_FREE(tmp);

    priv->monJSON = virXPathBoolean("count(./monitor[@json = '1']) > 0",
                                    ctxt) > 0;

    switch (priv->monConfig->type) {
    case VIR_DOMAIN_CHR_TYPE_PTY:
        priv->monConfig->data.file.path = monitorpath;
        break;
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        priv->monConfig->data.nix.path = monitorpath;
        break;
    default:
        VIR_FREE(monitorpath);
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unsupported monitor type '%s'"),
                       virDomainChrTypeToString(priv->monConfig->type));
        goto error;
    }

    if ((node = virXPathNode("./namespaces", ctxt))) {
        xmlNodePtr next;

        for (next = node->children; next; next = next->next) {
            int ns = qemuDomainNamespaceTypeFromString((const char *)next->name);

            if (ns < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("malformed namespace name: %s"),
                               next->name);
                goto error;
            }

            if (qemuDomainEnableNamespace(vm, ns) < 0)
                goto error;
        }
    }

    if (priv->namespaces &&
        virBitmapIsAllClear(priv->namespaces)) {
        virBitmapFree(priv->namespaces);
        priv->namespaces = NULL;
    }

    if ((n = virXPathNodeSet("./vcpus/vcpu", ctxt, &nodes)) < 0)
        goto error;

    for (i = 0; i < n; i++) {
        if (qemuDomainObjPrivateXMLParseVcpu(nodes[i], i, vm->def) < 0)
            goto error;
    }
    VIR_FREE(nodes);

    if ((n = virXPathNodeSet("./qemuCaps/flag", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("failed to parse qemu capabilities flags"));
        goto error;
    }
    if (n > 0) {
        if (!(qemuCaps = virQEMUCapsNew()))
            goto error;

        for (i = 0; i < n; i++) {
            char *str = virXMLPropString(nodes[i], "name");
            if (str) {
                int flag = virQEMUCapsTypeFromString(str);
                if (flag < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Unknown qemu capabilities flag %s"), str);
                    VIR_FREE(str);
                    goto error;
                }
                VIR_FREE(str);
                virQEMUCapsSet(qemuCaps, flag);
            }
        }

        priv->qemuCaps = qemuCaps;
        qemuCaps = NULL;
    }
    VIR_FREE(nodes);

    priv->lockState = virXPathString("string(./lockstate)", ctxt);

    if (qemuDomainObjPrivateXMLParseJob(vm, priv, ctxt) < 0)
        goto error;

    priv->fakeReboot = virXPathBoolean("boolean(./fakereboot)", ctxt) == 1;

    if ((n = virXPathNodeSet("./devices/device", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse qemu device list"));
        goto error;
    }
    if (n > 0) {
        /* NULL-terminated list */
        if (VIR_ALLOC_N(priv->qemuDevices, n + 1) < 0)
            goto error;

        for (i = 0; i < n; i++) {
            priv->qemuDevices[i] = virXMLPropString(nodes[i], "alias");
            if (!priv->qemuDevices[i]) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("failed to parse qemu device list"));
                goto error;
            }
        }
    }
    VIR_FREE(nodes);

    if (qemuDomainObjPrivateXMLParseAutomaticPlacement(ctxt, priv, driver) < 0)
        goto error;

    if ((tmp = virXPathString("string(./libDir/@path)", ctxt)))
        priv->libDir = tmp;
    if ((tmp = virXPathString("string(./channelTargetDir/@path)", ctxt)))
        priv->channelTargetDir = tmp;
    tmp = NULL;

    if (qemuDomainSetPrivatePathsOld(driver, vm) < 0)
        goto error;

    if (virCPUDefParseXML(ctxt, "./cpu", VIR_CPU_TYPE_GUEST, &priv->origCPU) < 0)
        goto error;

    priv->chardevStdioLogd = virXPathBoolean("boolean(./chardevStdioLogd)",
                                             ctxt) == 1;

    qemuDomainObjPrivateXMLParseAllowReboot(ctxt, &priv->allowReboot);

    if (qemuDomainObjPrivateXMLParseBlockjobs(priv, ctxt) < 0)
        goto error;

    return 0;

 error:
    VIR_FREE(nodes);
    VIR_FREE(tmp);
    virBitmapFree(priv->namespaces);
    priv->namespaces = NULL;
    virDomainChrSourceDefFree(priv->monConfig);
    priv->monConfig = NULL;
    virStringListFree(priv->qemuDevices);
    priv->qemuDevices = NULL;
    virObjectUnref(qemuCaps);
    return -1;
}


virDomainXMLPrivateDataCallbacks virQEMUDriverPrivateDataCallbacks = {
    .alloc = qemuDomainObjPrivateAlloc,
    .free = qemuDomainObjPrivateFree,
    .diskNew = qemuDomainDiskPrivateNew,
    .vcpuNew = qemuDomainVcpuPrivateNew,
    .chrSourceNew = qemuDomainChrSourcePrivateNew,
    .parse = qemuDomainObjPrivateXMLParse,
    .format = qemuDomainObjPrivateXMLFormat,
    .storageParse = qemuStorageSourcePrivateDataParse,
    .storageFormat = qemuStorageSourcePrivateDataFormat,
};


static void
qemuDomainDefNamespaceFree(void *nsdata)
{
    qemuDomainCmdlineDefPtr cmd = nsdata;

    qemuDomainCmdlineDefFree(cmd);
}

static int
qemuDomainDefNamespaceParse(xmlDocPtr xml ATTRIBUTE_UNUSED,
                            xmlNodePtr root ATTRIBUTE_UNUSED,
                            xmlXPathContextPtr ctxt,
                            void **data)
{
    qemuDomainCmdlineDefPtr cmd = NULL;
    bool uses_qemu_ns = false;
    xmlNodePtr *nodes = NULL;
    int n;
    size_t i;

    if (xmlXPathRegisterNs(ctxt, BAD_CAST "qemu", BAD_CAST QEMU_NAMESPACE_HREF) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to register xml namespace '%s'"),
                       QEMU_NAMESPACE_HREF);
        return -1;
    }

    if (VIR_ALLOC(cmd) < 0)
        return -1;

    /* first handle the extra command-line arguments */
    n = virXPathNodeSet("./qemu:commandline/qemu:arg", ctxt, &nodes);
    if (n < 0)
        goto error;
    uses_qemu_ns |= n > 0;

    if (n && VIR_ALLOC_N(cmd->args, n) < 0)
        goto error;

    for (i = 0; i < n; i++) {
        cmd->args[cmd->num_args] = virXMLPropString(nodes[i], "value");
        if (cmd->args[cmd->num_args] == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("No qemu command-line argument specified"));
            goto error;
        }
        cmd->num_args++;
    }

    VIR_FREE(nodes);

    /* now handle the extra environment variables */
    n = virXPathNodeSet("./qemu:commandline/qemu:env", ctxt, &nodes);
    if (n < 0)
        goto error;
    uses_qemu_ns |= n > 0;

    if (n && VIR_ALLOC_N(cmd->env_name, n) < 0)
        goto error;

    if (n && VIR_ALLOC_N(cmd->env_value, n) < 0)
        goto error;

    for (i = 0; i < n; i++) {
        char *tmp;

        tmp = virXMLPropString(nodes[i], "name");
        if (tmp == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("No qemu environment name specified"));
            goto error;
        }
        if (tmp[0] == '\0') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Empty qemu environment name specified"));
            goto error;
        }
        if (!c_isalpha(tmp[0]) && tmp[0] != '_') {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Invalid environment name, it must begin with a letter or underscore"));
            goto error;
        }
        if (strspn(tmp, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(tmp)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("Invalid environment name, it must contain only alphanumerics and underscore"));
            goto error;
        }

        cmd->env_name[cmd->num_env] = tmp;

        cmd->env_value[cmd->num_env] = virXMLPropString(nodes[i], "value");
        /* a NULL value for command is allowed, since it might be empty */
        cmd->num_env++;
    }

    VIR_FREE(nodes);

    if (uses_qemu_ns)
        *data = cmd;
    else
        VIR_FREE(cmd);

    return 0;

 error:
    VIR_FREE(nodes);
    qemuDomainDefNamespaceFree(cmd);
    return -1;
}

static int
qemuDomainDefNamespaceFormatXML(virBufferPtr buf,
                                void *nsdata)
{
    qemuDomainCmdlineDefPtr cmd = nsdata;
    size_t i;

    if (!cmd->num_args && !cmd->num_env)
        return 0;

    virBufferAddLit(buf, "<qemu:commandline>\n");
    virBufferAdjustIndent(buf, 2);

    for (i = 0; i < cmd->num_args; i++)
        virBufferEscapeString(buf, "<qemu:arg value='%s'/>\n",
                              cmd->args[i]);
    for (i = 0; i < cmd->num_env; i++) {
        virBufferAsprintf(buf, "<qemu:env name='%s'", cmd->env_name[i]);
        if (cmd->env_value[i])
            virBufferEscapeString(buf, " value='%s'", cmd->env_value[i]);
        virBufferAddLit(buf, "/>\n");
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</qemu:commandline>\n");
    return 0;
}

static const char *
qemuDomainDefNamespaceHref(void)
{
    return "xmlns:qemu='" QEMU_NAMESPACE_HREF "'";
}


virDomainXMLNamespace virQEMUDriverDomainXMLNamespace = {
    .parse = qemuDomainDefNamespaceParse,
    .free = qemuDomainDefNamespaceFree,
    .format = qemuDomainDefNamespaceFormatXML,
    .href = qemuDomainDefNamespaceHref,
};


static int
qemuDomainDefAddImplicitInputDevice(virDomainDef *def)
{
    if (ARCH_IS_X86(def->os.arch)) {
        if (virDomainDefMaybeAddInput(def,
                                      VIR_DOMAIN_INPUT_TYPE_MOUSE,
                                      VIR_DOMAIN_INPUT_BUS_PS2) < 0)
            return -1;

        if (virDomainDefMaybeAddInput(def,
                                      VIR_DOMAIN_INPUT_TYPE_KBD,
                                      VIR_DOMAIN_INPUT_BUS_PS2) < 0)
            return -1;
    }

    return 0;
}


static int
qemuDomainDefAddDefaultDevices(virDomainDefPtr def,
                               virQEMUCapsPtr qemuCaps)
{
    bool addDefaultUSB = true;
    int usbModel = -1; /* "default for machinetype" */
    int pciRoot;       /* index within def->controllers */
    bool addImplicitSATA = false;
    bool addPCIRoot = false;
    bool addPCIeRoot = false;
    bool addDefaultMemballoon = true;
    bool addDefaultUSBKBD = false;
    bool addDefaultUSBMouse = false;
    bool addPanicDevice = false;
    int ret = -1;

    /* add implicit input devices */
    if (qemuDomainDefAddImplicitInputDevice(def) < 0)
        goto cleanup;

    /* Add implicit PCI root controller if the machine has one */
    switch (def->os.arch) {
    case VIR_ARCH_I686:
    case VIR_ARCH_X86_64:
        if (STREQ(def->os.machine, "isapc")) {
            addDefaultUSB = false;
            break;
        }
        if (qemuDomainIsQ35(def)) {
            addPCIeRoot = true;
            addImplicitSATA = true;

            /* Prefer adding a USB3 controller if supported, fall back
             * to USB2 if there is no USB3 available, and if that's
             * unavailable don't add anything.
             */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QEMU_XHCI))
                usbModel = VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI;
            else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NEC_USB_XHCI))
                usbModel = VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI;
            else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_USB_EHCI1))
                usbModel = VIR_DOMAIN_CONTROLLER_MODEL_USB_ICH9_EHCI1;
            else
                addDefaultUSB = false;
            break;
        }
        if (qemuDomainIsI440FX(def))
            addPCIRoot = true;
        break;

    case VIR_ARCH_ARMV7L:
    case VIR_ARCH_AARCH64:
        addDefaultUSB = false;
        addDefaultMemballoon = false;
        if (qemuDomainIsVirt(def))
            addPCIeRoot = virQEMUCapsGet(qemuCaps, QEMU_CAPS_OBJECT_GPEX);
        break;

    case VIR_ARCH_PPC64:
    case VIR_ARCH_PPC64LE:
        addPCIRoot = true;
        addDefaultUSBKBD = true;
        addDefaultUSBMouse = true;
        /* For pSeries guests, the firmware provides the same
         * functionality as the pvpanic device, so automatically
         * add the definition if not already present */
        if (qemuDomainIsPSeries(def))
            addPanicDevice = true;
        break;

    case VIR_ARCH_ALPHA:
    case VIR_ARCH_PPC:
    case VIR_ARCH_PPCEMB:
    case VIR_ARCH_SH4:
    case VIR_ARCH_SH4EB:
        addPCIRoot = true;
        break;

    case VIR_ARCH_S390:
    case VIR_ARCH_S390X:
        addDefaultUSB = false;
        addPanicDevice = true;
        break;

    case VIR_ARCH_SPARC:
    case VIR_ARCH_SPARC64:
        addPCIRoot = true;
        break;

    case VIR_ARCH_ARMV6L:
    case VIR_ARCH_ARMV7B:
    case VIR_ARCH_CRIS:
    case VIR_ARCH_ITANIUM:
    case VIR_ARCH_LM32:
    case VIR_ARCH_M68K:
    case VIR_ARCH_MICROBLAZE:
    case VIR_ARCH_MICROBLAZEEL:
    case VIR_ARCH_MIPS:
    case VIR_ARCH_MIPSEL:
    case VIR_ARCH_MIPS64:
    case VIR_ARCH_MIPS64EL:
    case VIR_ARCH_OR32:
    case VIR_ARCH_PARISC:
    case VIR_ARCH_PARISC64:
    case VIR_ARCH_PPCLE:
    case VIR_ARCH_UNICORE32:
    case VIR_ARCH_XTENSA:
    case VIR_ARCH_XTENSAEB:
    case VIR_ARCH_NONE:
    case VIR_ARCH_LAST:
    default:
        break;
    }

    if (addDefaultUSB &&
        virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_USB, 0) < 0 &&
        virDomainDefAddUSBController(def, 0, usbModel) < 0)
        goto cleanup;

    if (addImplicitSATA &&
        virDomainDefMaybeAddController(
            def, VIR_DOMAIN_CONTROLLER_TYPE_SATA, 0, -1) < 0)
        goto cleanup;

    pciRoot = virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0);

    /* NB: any machine that sets addPCIRoot to true must also return
     * true from the function qemuDomainSupportsPCI().
     */
    if (addPCIRoot) {
        if (pciRoot >= 0) {
            if (def->controllers[pciRoot]->model != VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("The PCI controller with index='0' must be "
                                 "model='pci-root' for this machine type, "
                                 "but model='%s' was found instead"),
                               virDomainControllerModelPCITypeToString(def->controllers[pciRoot]->model));
                goto cleanup;
            }
        } else if (!virDomainDefAddController(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0,
                                              VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT)) {
            goto cleanup;
        }
    }

    /* When a machine has a pcie-root, make sure that there is always
     * a dmi-to-pci-bridge controller added as bus 1, and a pci-bridge
     * as bus 2, so that standard PCI devices can be connected
     *
     * NB: any machine that sets addPCIeRoot to true must also return
     * true from the function qemuDomainSupportsPCI().
     */
    if (addPCIeRoot) {
        if (pciRoot >= 0) {
            if (def->controllers[pciRoot]->model != VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("The PCI controller with index='0' must be "
                                 "model='pcie-root' for this machine type, "
                                 "but model='%s' was found instead"),
                               virDomainControllerModelPCITypeToString(def->controllers[pciRoot]->model));
                goto cleanup;
            }
        } else if (!virDomainDefAddController(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0,
                                             VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT)) {
            goto cleanup;
        }
    }

    if (addDefaultMemballoon && !def->memballoon) {
        virDomainMemballoonDefPtr memballoon;
        if (VIR_ALLOC(memballoon) < 0)
            goto cleanup;

        memballoon->model = VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO;
        def->memballoon = memballoon;
    }

    if (addDefaultUSBKBD &&
        def->ngraphics > 0 &&
        virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_KBD,
                                  VIR_DOMAIN_INPUT_BUS_USB) < 0)
        goto cleanup;

    if (addDefaultUSBMouse &&
        def->ngraphics > 0 &&
        virDomainDefMaybeAddInput(def,
                                  VIR_DOMAIN_INPUT_TYPE_MOUSE,
                                  VIR_DOMAIN_INPUT_BUS_USB) < 0)
        goto cleanup;

    if (addPanicDevice) {
        size_t j;
        for (j = 0; j < def->npanics; j++) {
            if (def->panics[j]->model == VIR_DOMAIN_PANIC_MODEL_DEFAULT ||
                (ARCH_IS_PPC64(def->os.arch) &&
                     def->panics[j]->model == VIR_DOMAIN_PANIC_MODEL_PSERIES) ||
                (ARCH_IS_S390(def->os.arch) &&
                     def->panics[j]->model == VIR_DOMAIN_PANIC_MODEL_S390))
                break;
        }

        if (j == def->npanics) {
            virDomainPanicDefPtr panic;
            if (VIR_ALLOC(panic) < 0 ||
                VIR_APPEND_ELEMENT_COPY(def->panics,
                                        def->npanics, panic) < 0) {
                VIR_FREE(panic);
                goto cleanup;
            }
        }
    }

    ret = 0;
 cleanup:
    return ret;
}


/**
 * qemuDomainDefEnableDefaultFeatures:
 * @def: domain definition
 * @qemuCaps: QEMU capabilities
 *
 * Make sure that features that should be enabled by default are actually
 * enabled and configure default values related to those features.
 */
static void
qemuDomainDefEnableDefaultFeatures(virDomainDefPtr def,
                                   virQEMUCapsPtr qemuCaps)
{
    /* The virt machine type always uses GIC: if the relevant information
     * was not included in the domain XML, we need to choose a suitable
     * GIC version ourselves */
    if ((def->features[VIR_DOMAIN_FEATURE_GIC] == VIR_TRISTATE_SWITCH_ABSENT &&
         qemuDomainIsVirt(def)) ||
        (def->features[VIR_DOMAIN_FEATURE_GIC] == VIR_TRISTATE_SWITCH_ON &&
         def->gic_version == VIR_GIC_VERSION_NONE)) {
        virGICVersion version;

        VIR_DEBUG("Looking for usable GIC version in domain capabilities");
        for (version = VIR_GIC_VERSION_LAST - 1;
             version > VIR_GIC_VERSION_NONE;
             version--) {

            /* We want to use the highest available GIC version for guests;
             * however, the emulated GICv3 is currently lacking a MSI controller,
             * making it unsuitable for the pure PCIe topology we aim for.
             *
             * For that reason, we skip this step entirely for TCG guests,
             * and rely on the code below to pick the default version, GICv2,
             * which supports all the features we need.
             *
             * See https://bugzilla.redhat.com/show_bug.cgi?id=1414081 */
            if (version == VIR_GIC_VERSION_3 &&
                def->virtType == VIR_DOMAIN_VIRT_QEMU) {
                continue;
            }

            if (virQEMUCapsSupportsGICVersion(qemuCaps,
                                              def->virtType,
                                              version)) {
                VIR_DEBUG("Using GIC version %s",
                          virGICVersionTypeToString(version));
                def->gic_version = version;
                break;
            }
        }

        /* Use the default GIC version (GICv2) as a last-ditch attempt
         * if no match could be found above */
        if (def->gic_version == VIR_GIC_VERSION_NONE) {
            VIR_DEBUG("Using GIC version 2 (default)");
            def->gic_version = VIR_GIC_VERSION_2;
        }

        /* Even if we haven't found a usable GIC version in the domain
         * capabilities, we still want to enable this */
        def->features[VIR_DOMAIN_FEATURE_GIC] = VIR_TRISTATE_SWITCH_ON;
    }
}


static int
qemuCanonicalizeMachine(virDomainDefPtr def, virQEMUCapsPtr qemuCaps)
{
    const char *canon;

    if (!(canon = virQEMUCapsGetCanonicalMachine(qemuCaps, def->os.machine)))
        return 0;

    if (STRNEQ(canon, def->os.machine)) {
        char *tmp;
        if (VIR_STRDUP(tmp, canon) < 0)
            return -1;
        VIR_FREE(def->os.machine);
        def->os.machine = tmp;
    }

    return 0;
}


static int
qemuDomainRecheckInternalPaths(virDomainDefPtr def,
                               virQEMUDriverConfigPtr cfg,
                               unsigned int flags)
{
    size_t i = 0;
    size_t j = 0;

    for (i = 0; i < def->ngraphics; ++i) {
        virDomainGraphicsDefPtr graphics = def->graphics[i];

        for (j = 0; j < graphics->nListens; ++j) {
            virDomainGraphicsListenDefPtr glisten =  &graphics->listens[j];

            /* This will happen only if we parse XML from old libvirts where
             * unix socket was available only for VNC graphics.  In this
             * particular case we should follow the behavior and if we remove
             * the auto-generated socket based on config option from qemu.conf
             * we need to change the listen type to address. */
            if (graphics->type == VIR_DOMAIN_GRAPHICS_TYPE_VNC &&
                glisten->type == VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET &&
                glisten->socket &&
                !glisten->autoGenerated &&
                STRPREFIX(glisten->socket, cfg->libDir)) {
                if (flags & VIR_DOMAIN_DEF_PARSE_INACTIVE) {
                    VIR_FREE(glisten->socket);
                    glisten->type = VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS;
                } else {
                    glisten->fromConfig = true;
                }
            }
        }
    }

    return 0;
}


static int
qemuDomainDefVcpusPostParse(virDomainDefPtr def)
{
    unsigned int maxvcpus = virDomainDefGetVcpusMax(def);
    virDomainVcpuDefPtr vcpu;
    virDomainVcpuDefPtr prevvcpu;
    size_t i;
    bool has_order = false;

    /* vcpu 0 needs to be present, first, and non-hotpluggable */
    vcpu = virDomainDefGetVcpu(def, 0);
    if (!vcpu->online) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vcpu 0 can't be offline"));
        return -1;
    }
    if (vcpu->hotpluggable == VIR_TRISTATE_BOOL_YES) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vcpu0 can't be hotpluggable"));
        return -1;
    }
    if (vcpu->order != 0 && vcpu->order != 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("vcpu0 must be enabled first"));
        return -1;
    }

    if (vcpu->order != 0)
        has_order = true;

    prevvcpu = vcpu;

    /* all online vcpus or non online vcpu need to have order set */
    for (i = 1; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);

        if (vcpu->online &&
            (vcpu->order != 0) != has_order) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("all vcpus must have either set or unset order"));
            return -1;
        }

        /* few conditions for non-hotpluggable (thus online) vcpus */
        if (vcpu->hotpluggable == VIR_TRISTATE_BOOL_NO) {
            /* they can be ordered only at the beginning */
            if (prevvcpu->hotpluggable == VIR_TRISTATE_BOOL_YES) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("online non-hotpluggable vcpus need to be "
                                 "ordered prior to hotplugable vcpus"));
                return -1;
            }

            /* they need to be in order (qemu doesn't support any order yet).
             * Also note that multiple vcpus may share order on some platforms */
            if (prevvcpu->order > vcpu->order) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("online non-hotpluggable vcpus must be ordered "
                                 "in ascending order"));
                return -1;
            }
        }

        prevvcpu = vcpu;
    }

    return 0;
}


static int
qemuDomainDefCPUPostParse(virDomainDefPtr def)
{
    if (!def->cpu)
        return 0;

    if (def->cpu->cache) {
        virCPUCacheDefPtr cache = def->cpu->cache;

        if (!ARCH_IS_X86(def->os.arch)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("CPU cache specification is not supported "
                             "for '%s' architecture"),
                           virArchToString(def->os.arch));
            return -1;
        }

        switch (cache->mode) {
        case VIR_CPU_CACHE_MODE_EMULATE:
            if (cache->level != 3) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("CPU cache mode '%s' can only be used with "
                                 "level='3'"),
                               virCPUCacheModeTypeToString(cache->mode));
                return -1;
            }
            break;

        case VIR_CPU_CACHE_MODE_PASSTHROUGH:
            if (def->cpu->mode != VIR_CPU_MODE_HOST_PASSTHROUGH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("CPU cache mode '%s' can only be used with "
                                 "'%s' CPUs"),
                               virCPUCacheModeTypeToString(cache->mode),
                               virCPUModeTypeToString(VIR_CPU_MODE_HOST_PASSTHROUGH));
                return -1;
            }

            if (cache->level != -1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported CPU cache level for mode '%s'"),
                               virCPUCacheModeTypeToString(cache->mode));
                return -1;
            }
            break;

        case VIR_CPU_CACHE_MODE_DISABLE:
            if (cache->level != -1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported CPU cache level for mode '%s'"),
                               virCPUCacheModeTypeToString(cache->mode));
                return -1;
            }
            break;

        case VIR_CPU_CACHE_MODE_LAST:
            break;
        }
    }

    /* Nothing to be done if only CPU topology is specified. */
    if (def->cpu->mode == VIR_CPU_MODE_CUSTOM &&
        !def->cpu->model)
        return 0;

    if (def->cpu->check != VIR_CPU_CHECK_DEFAULT)
        return 0;

    switch ((virCPUMode) def->cpu->mode) {
    case VIR_CPU_MODE_HOST_PASSTHROUGH:
        def->cpu->check = VIR_CPU_CHECK_NONE;
        break;

    case VIR_CPU_MODE_HOST_MODEL:
        def->cpu->check = VIR_CPU_CHECK_PARTIAL;
        break;

    case VIR_CPU_MODE_CUSTOM:
        /* Custom CPUs in TCG mode are not compared to host CPU by default. */
        if (def->virtType == VIR_DOMAIN_VIRT_QEMU)
            def->cpu->check = VIR_CPU_CHECK_NONE;
        else
            def->cpu->check = VIR_CPU_CHECK_PARTIAL;
        break;

    case VIR_CPU_MODE_LAST:
        break;
    }

    return 0;
}


static int
qemuDomainDefPostParseBasic(virDomainDefPtr def,
                            virCapsPtr caps,
                            void *opaque ATTRIBUTE_UNUSED)
{
    /* check for emulator and create a default one if needed */
    if (!def->emulator &&
        !(def->emulator = virDomainDefGetDefaultEmulator(def, caps)))
        return 1;

    return 0;
}


static int
qemuDomainDefPostParse(virDomainDefPtr def,
                       virCapsPtr caps ATTRIBUTE_UNUSED,
                       unsigned int parseFlags,
                       void *opaque,
                       void *parseOpaque)
{
    virQEMUDriverPtr driver = opaque;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    /* Note that qemuCaps may be NULL when this function is called. This
     * function shall not fail in that case. It will be re-run on VM startup
     * with the capabilities populated. */
    virQEMUCapsPtr qemuCaps = parseOpaque;
    int ret = -1;

    if (def->os.bootloader || def->os.bootloaderArgs) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("bootloader is not supported by QEMU"));
        goto cleanup;
    }

    if (!def->os.machine) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing machine type"));
        goto cleanup;
    }

    if (def->os.loader &&
        def->os.loader->type == VIR_DOMAIN_LOADER_TYPE_PFLASH &&
        def->os.loader->readonly == VIR_TRISTATE_SWITCH_ON &&
        !def->os.loader->nvram) {
        if (virAsprintf(&def->os.loader->nvram, "%s/%s_VARS.fd",
                        cfg->nvramDir, def->name) < 0)
            goto cleanup;
    }

    if (qemuDomainDefAddDefaultDevices(def, qemuCaps) < 0)
        goto cleanup;

    if (qemuCanonicalizeMachine(def, qemuCaps) < 0)
        goto cleanup;

    qemuDomainDefEnableDefaultFeatures(def, qemuCaps);

    if (qemuDomainRecheckInternalPaths(def, cfg, parseFlags) < 0)
        goto cleanup;

    if (qemuSecurityVerify(driver->securityManager, def) < 0)
        goto cleanup;

    if (qemuDomainDefVcpusPostParse(def) < 0)
        goto cleanup;

    if (qemuDomainDefCPUPostParse(def) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}


/**
 * qemuDomainDefGetVcpuHotplugGranularity:
 * @def: domain definition
 *
 * With QEMU 2.7 and newer, vCPUs can only be hotplugged in groups that
 * respect the guest's hotplug granularity; because of that, QEMU will
 * not allow guests to start unless the initial number of vCPUs is a
 * multiple of the hotplug granularity.
 *
 * Returns the vCPU hotplug granularity.
 */
static unsigned int
qemuDomainDefGetVcpuHotplugGranularity(const virDomainDef *def)
{
    /* If the guest CPU topology has not been configured, assume we
     * can hotplug vCPUs one at a time */
    if (!def->cpu || def->cpu->sockets == 0)
        return 1;

    /* For pSeries guests, hotplug can only be performed one core
     * at a time, so the vCPU hotplug granularity is the number
     * of threads per core */
    if (qemuDomainIsPSeries(def))
        return def->cpu->threads;

    /* In all other cases, we can hotplug vCPUs one at a time */
    return 1;
}


#define QEMU_MAX_VCPUS_WITHOUT_EIM 255


static int
qemuDomainDefValidateFeatures(const virDomainDef *def)
{
    size_t i;

    for (i = 0; i < VIR_DOMAIN_FEATURE_LAST; i++) {
        const char *featureName = virDomainFeatureTypeToString(i);

        switch ((virDomainFeature) i) {
        case VIR_DOMAIN_FEATURE_IOAPIC:
            if (def->features[i] != VIR_DOMAIN_IOAPIC_NONE &&
                !ARCH_IS_X86(def->os.arch)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("The '%s' feature is not supported for "
                                 "architecture '%s' or machine type '%s'"),
                               featureName,
                               virArchToString(def->os.arch),
                               def->os.machine);
                return -1;
            }
            break;

        case VIR_DOMAIN_FEATURE_HPT:
            if (def->features[i] != VIR_DOMAIN_HPT_RESIZING_NONE &&
                !qemuDomainIsPSeries(def)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("The '%s' feature is not supported for "
                                 "architecture '%s' or machine type '%s'"),
                               featureName,
                               virArchToString(def->os.arch),
                               def->os.machine);
                return -1;
            }
            break;

        case VIR_DOMAIN_FEATURE_GIC:
            if (def->features[i] == VIR_TRISTATE_SWITCH_ON &&
                !qemuDomainIsVirt(def)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("The '%s' feature is not supported for "
                                 "architecture '%s' or machine type '%s'"),
                               featureName,
                               virArchToString(def->os.arch),
                               def->os.machine);
                return -1;
            }
            break;

        case VIR_DOMAIN_FEATURE_ACPI:
        case VIR_DOMAIN_FEATURE_APIC:
        case VIR_DOMAIN_FEATURE_PAE:
        case VIR_DOMAIN_FEATURE_HAP:
        case VIR_DOMAIN_FEATURE_VIRIDIAN:
        case VIR_DOMAIN_FEATURE_PRIVNET:
        case VIR_DOMAIN_FEATURE_HYPERV:
        case VIR_DOMAIN_FEATURE_KVM:
        case VIR_DOMAIN_FEATURE_PVSPINLOCK:
        case VIR_DOMAIN_FEATURE_CAPABILITIES:
        case VIR_DOMAIN_FEATURE_PMU:
        case VIR_DOMAIN_FEATURE_VMPORT:
        case VIR_DOMAIN_FEATURE_SMM:
        case VIR_DOMAIN_FEATURE_VMCOREINFO:
        case VIR_DOMAIN_FEATURE_LAST:
            break;
        }
    }

    return 0;
}


static int
qemuDomainDefValidate(const virDomainDef *def,
                      virCapsPtr caps ATTRIBUTE_UNUSED,
                      void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    virQEMUCapsPtr qemuCaps = NULL;
    int ret = -1;

    if (!(qemuCaps = virQEMUCapsCacheLookup(driver->qemuCapsCache,
                                            def->emulator)))
        goto cleanup;

    if (def->mem.min_guarantee) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Parameter 'min_guarantee' not supported by QEMU."));
        goto cleanup;
    }

    /* On x86, UEFI requires ACPI */
    if (def->os.loader &&
        def->os.loader->type == VIR_DOMAIN_LOADER_TYPE_PFLASH &&
        ARCH_IS_X86(def->os.arch) &&
        def->features[VIR_DOMAIN_FEATURE_ACPI] != VIR_TRISTATE_SWITCH_ON) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("UEFI requires ACPI on this architecture"));
        goto cleanup;
    }

    /* On aarch64, ACPI requires UEFI */
    if (def->features[VIR_DOMAIN_FEATURE_ACPI] == VIR_TRISTATE_SWITCH_ON &&
        def->os.arch == VIR_ARCH_AARCH64 &&
        (!def->os.loader ||
         def->os.loader->type != VIR_DOMAIN_LOADER_TYPE_PFLASH)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("ACPI requires UEFI on this architecture"));
        goto cleanup;
    }

    if (def->os.loader &&
        def->os.loader->secure == VIR_TRISTATE_BOOL_YES) {
        /* These are the QEMU implementation limitations. But we
         * have to live with them for now. */

        if (!qemuDomainIsQ35(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Secure boot is supported with q35 machine types only"));
            goto cleanup;
        }

        /* Now, technically it is possible to have secure boot on
         * 32bits too, but that would require some -cpu xxx magic
         * too. Not worth it unless we are explicitly asked. */
        if (def->os.arch != VIR_ARCH_X86_64) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Secure boot is supported for x86_64 architecture only"));
            goto cleanup;
        }

        if (def->features[VIR_DOMAIN_FEATURE_SMM] != VIR_TRISTATE_SWITCH_ON) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Secure boot requires SMM feature enabled"));
            goto cleanup;
        }
    }

    /* QEMU 2.7 (detected via the availability of query-hotpluggable-cpus)
     * enforces stricter rules than previous versions when it comes to guest
     * CPU topology. Verify known constraints are respected */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_HOTPLUGGABLE_CPUS)) {
        unsigned int topologycpus;
        unsigned int granularity;

        /* Starting from QEMU 2.5, max vCPU count and overall vCPU topology
         * must agree. We only actually enforce this with QEMU 2.7+, due
         * to the capability check above */
        if (virDomainDefGetVcpusTopology(def, &topologycpus) == 0 &&
            topologycpus != virDomainDefGetVcpusMax(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CPU topology doesn't match maximum vcpu count"));
            goto cleanup;
        }

        /* vCPU hotplug granularity must be respected */
        granularity = qemuDomainDefGetVcpuHotplugGranularity(def);
        if ((virDomainDefGetVcpus(def) % granularity) != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("vCPUs count must be a multiple of the vCPU "
                             "hotplug granularity (%u)"),
                           granularity);
            goto cleanup;
        }
    }

    if (ARCH_IS_X86(def->os.arch) &&
        virDomainDefGetVcpusMax(def) > QEMU_MAX_VCPUS_WITHOUT_EIM) {
        if (!qemuDomainIsQ35(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("more than %d vCPUs are only supported on "
                             "q35-based machine types"),
                           QEMU_MAX_VCPUS_WITHOUT_EIM);
            goto cleanup;
        }
        if (!def->iommu || def->iommu->eim != VIR_TRISTATE_SWITCH_ON) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("more than %d vCPUs require extended interrupt "
                             "mode enabled on the iommu device"),
                           QEMU_MAX_VCPUS_WITHOUT_EIM);
            goto cleanup;
        }
    }

    if (qemuDomainDefValidateFeatures(def) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virObjectUnref(qemuCaps);
    return ret;
}


static bool
qemuDomainNetSupportsCoalesce(virDomainNetType type)
{
    switch (type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
        return true;
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }
    return false;
}


static int
qemuDomainChrSourceReconnectDefValidate(const virDomainChrSourceReconnectDef *def)
{
    if (def->enabled == VIR_TRISTATE_BOOL_YES &&
        def->timeout == 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("chardev reconnect source timeout cannot be '0'"));
        return -1;
    }

    return 0;
}


static int
qemuDomainChrSourceDefValidate(const virDomainChrSourceDef *def)
{
    switch ((virDomainChrType)def->type) {
    case VIR_DOMAIN_CHR_TYPE_TCP:
        if (qemuDomainChrSourceReconnectDefValidate(&def->data.tcp.reconnect) < 0)
            return -1;
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (qemuDomainChrSourceReconnectDefValidate(&def->data.nix.reconnect) < 0)
            return -1;
        break;

    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_FILE:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        break;
    }

    return 0;
}


static int
qemuDomainChrSerialTargetTypeToAddressType(int targetType)
{
    switch ((virDomainChrSerialTargetType)targetType) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA:
        return VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB:
        return VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI:
        return VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO:
        return VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_LAST:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE:
        break;
    }

    return VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE;
}


static int
qemuDomainChrSerialTargetModelToTargetType(int targetModel)
{
    switch ((virDomainChrSerialTargetModel) targetModel) {
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:
        return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP;
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
    case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
        break;
    }

    return VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE;
}


static int
qemuDomainChrTargetDefValidate(const virDomainChrDef *chr)
{
    int expected;

    switch ((virDomainChrDeviceType)chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:

        /* Validate target type */
        switch ((virDomainChrSerialTargetType)chr->targetType) {
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO:

            expected = qemuDomainChrSerialTargetTypeToAddressType(chr->targetType);

            if (chr->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
                chr->info.type != expected) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("Target type '%s' requires address type '%s'"),
                               virDomainChrSerialTargetTypeToString(chr->targetType),
                               virDomainDeviceAddressTypeToString(expected));
                return -1;
            }
            break;

        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP:
            if (chr->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("Target type '%s' cannot have an "
                                 "associated address"),
                               virDomainChrSerialTargetTypeToString(chr->targetType));
                return -1;
            }
            break;

        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_LAST:
            break;
        }

        /* Validate target model */
        switch ((virDomainChrSerialTargetModel) chr->targetModel) {
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE:

            expected = qemuDomainChrSerialTargetModelToTargetType(chr->targetModel);

            if (chr->targetType != expected) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("Target model '%s' requires target type '%s'"),
                               virDomainChrSerialTargetModelTypeToString(chr->targetModel),
                               virDomainChrSerialTargetTypeToString(expected));
                return -1;
            }
            break;

        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_LAST:
            break;
        }
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        /* Nothing to do */
        break;
    }

    return 0;
}


static int
qemuDomainChrDefValidate(const virDomainChrDef *dev,
                         const virDomainDef *def)
{
    if (qemuDomainChrSourceDefValidate(dev->source) < 0)
        return -1;

    if (qemuDomainChrTargetDefValidate(dev) < 0)
        return -1;

    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL &&
        (ARCH_IS_S390(def->os.arch) || qemuDomainIsPSeries(def))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("parallel ports are not supported"));
            return -1;
    }

    if (dev->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL) {
        bool isCompatible = true;

        if (!qemuDomainIsPSeries(def) &&
            (dev->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO ||
             dev->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY)) {
            isCompatible = false;
        }

        if (!qemuDomainIsVirt(def) &&
            (dev->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM ||
             dev->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011)) {
            isCompatible = false;
        }

        if (!ARCH_IS_S390(def->os.arch) &&
            (dev->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP ||
             dev->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE ||
             dev->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPLMCONSOLE)) {
            isCompatible = false;
        }

        if (!isCompatible) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Serial device with target type '%s' and "
                             "target model '%s' not compatible with guest "
                             "architecture or machine type"),
                           virDomainChrSerialTargetTypeToString(dev->targetType),
                           virDomainChrSerialTargetModelTypeToString(dev->targetModel));
            return -1;
        }
    }

    return 0;
}


static int
qemuDomainSmartcardDefValidate(const virDomainSmartcardDef *def)
{
    if (def->type == VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH &&
        qemuDomainChrSourceDefValidate(def->data.passthru) < 0)
        return -1;

    return 0;
}


static int
qemuDomainRNGDefValidate(const virDomainRNGDef *def)
{
    if (def->backend == VIR_DOMAIN_RNG_BACKEND_EGD &&
        qemuDomainChrSourceDefValidate(def->source.chardev) < 0)
        return -1;

    return 0;
}


static int
qemuDomainRedirdevDefValidate(const virDomainRedirdevDef *def)
{
    if (qemuDomainChrSourceDefValidate(def->source) < 0)
        return -1;

    return 0;
}


static int
qemuDomainWatchdogDefValidate(const virDomainWatchdogDef *dev,
                              const virDomainDef *def)
{
    switch ((virDomainWatchdogModel) dev->model) {
    case VIR_DOMAIN_WATCHDOG_MODEL_I6300ESB:
        if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
            dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s model of watchdog can go only on PCI bus"),
                           virDomainWatchdogModelTypeToString(dev->model));
            return -1;
        }
        break;

    case VIR_DOMAIN_WATCHDOG_MODEL_IB700:
        if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
            dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s model of watchdog can go only on ISA bus"),
                           virDomainWatchdogModelTypeToString(dev->model));
            return -1;
        }
        break;

    case VIR_DOMAIN_WATCHDOG_MODEL_DIAG288:
        if (dev->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s model of watchdog is virtual and cannot go on any bus."),
                           virDomainWatchdogModelTypeToString(dev->model));
            return -1;
        }
        if (!(ARCH_IS_S390(def->os.arch))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("%s model of watchdog is allowed for s390 and s390x only"),
                           virDomainWatchdogModelTypeToString(dev->model));
            return -1;
        }
        break;

    case VIR_DOMAIN_WATCHDOG_MODEL_LAST:
        break;
    }

    return 0;
}


static int
qemuDomainDeviceDefValidateNetwork(const virDomainNetDef *net)
{
    bool hasIPv4 = false;
    bool hasIPv6 = false;
    size_t i;

    if (net->type == VIR_DOMAIN_NET_TYPE_USER) {
        if (net->guestIP.nroutes) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Invalid attempt to set network interface "
                             "guest-side IP route, not supported by QEMU"));
            return -1;
        }

        for (i = 0; i < net->guestIP.nips; i++) {
            const virNetDevIPAddr *ip = net->guestIP.ips[i];

            if (VIR_SOCKET_ADDR_VALID(&net->guestIP.ips[i]->peer)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Invalid attempt to set peer IP for guest"));
                return -1;
            }

            if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET)) {
                if (hasIPv4) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("Only one IPv4 address per "
                                     "interface is allowed"));
                    return -1;
                }
                hasIPv4 = true;

                if (ip->prefix < 4 || ip->prefix > 27) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("invalid prefix, must be in range of 4-27"));
                    return -1;
                }
            }

            if (VIR_SOCKET_ADDR_IS_FAMILY(&ip->address, AF_INET6)) {
                if (hasIPv6) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("Only one IPv6 address per "
                                     "interface is allowed"));
                    return -1;
                }
                hasIPv6 = true;

                if (ip->prefix > 120) {
                    virReportError(VIR_ERR_XML_ERROR, "%s",
                                   _("prefix too long"));
                    return -1;
                }
            }
        }
    } else if (net->guestIP.nroutes || net->guestIP.nips) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Invalid attempt to set network interface "
                         "guest-side IP route and/or address info, "
                         "not supported by QEMU"));
        return -1;
    }

    if (STREQ_NULLABLE(net->model, "virtio")) {
        if (net->driver.virtio.rx_queue_size & (net->driver.virtio.rx_queue_size - 1)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("rx_queue_size has to be a power of two"));
            return -1;
        }
        if (net->driver.virtio.tx_queue_size & (net->driver.virtio.tx_queue_size - 1)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("tx_queue_size has to be a power of two"));
            return -1;
        }
    }

    if (net->mtu &&
        !qemuDomainNetSupportsMTU(net->type)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("setting MTU on interface type %s is not supported yet"),
                       virDomainNetTypeToString(net->type));
        return -1;
    }

    if (net->coalesce && !qemuDomainNetSupportsCoalesce(net->type)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("coalesce settings on interface type %s are not supported"),
                       virDomainNetTypeToString(net->type));
        return -1;
    }

    return 0;
}


static int
qemuDomainDeviceDefValidateHostdev(const virDomainHostdevDef *hostdev,
                                   const virDomainDef *def)
{
    /* forbid capabilities mode hostdev in this kind of hypervisor */
    if (hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("hostdev mode 'capabilities' is not "
                         "supported in %s"),
                       virDomainVirtTypeToString(def->virtType));
        return -1;
    }

    return 0;
}


static int
qemuDomainDeviceDefValidateVideo(const virDomainVideoDef *video)
{
    switch (video->type) {
    case VIR_DOMAIN_VIDEO_TYPE_XEN:
    case VIR_DOMAIN_VIDEO_TYPE_VBOX:
    case VIR_DOMAIN_VIDEO_TYPE_PARALLELS:
    case VIR_DOMAIN_VIDEO_TYPE_DEFAULT:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("video type '%s' is not supported with QEMU"),
                       virDomainVideoTypeToString(video->type));
        return -1;
    case VIR_DOMAIN_VIDEO_TYPE_VGA:
    case VIR_DOMAIN_VIDEO_TYPE_CIRRUS:
    case VIR_DOMAIN_VIDEO_TYPE_VMVGA:
    case VIR_DOMAIN_VIDEO_TYPE_QXL:
    case VIR_DOMAIN_VIDEO_TYPE_VIRTIO:
    case VIR_DOMAIN_VIDEO_TYPE_LAST:
        break;
    }

    if (!video->primary &&
        video->type != VIR_DOMAIN_VIDEO_TYPE_QXL &&
        video->type != VIR_DOMAIN_VIDEO_TYPE_VIRTIO) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("video type '%s' is only valid as primary "
                         "video device"),
                       virDomainVideoTypeToString(video->type));
        return -1;
    }

    if (video->accel && video->accel->accel2d == VIR_TRISTATE_SWITCH_ON) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("qemu does not support the accel2d setting"));
        return -1;
    }

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_QXL) {
        if (video->vram > (UINT_MAX / 1024)) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("value for 'vram' must be less than '%u'"),
                           UINT_MAX / 1024);
            return -1;
        }
        if (video->ram > (UINT_MAX / 1024)) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("value for 'ram' must be less than '%u'"),
                           UINT_MAX / 1024);
            return -1;
        }
        if (video->vgamem) {
            if (video->vgamem < 1024) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("value for 'vgamem' must be at least 1 MiB "
                                 "(1024 KiB)"));
                return -1;
            }

            if (video->vgamem != VIR_ROUND_UP_POWER_OF_TWO(video->vgamem)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("value for 'vgamem' must be power of two"));
                return -1;
            }
        }
    }

    if (video->type == VIR_DOMAIN_VIDEO_TYPE_VGA ||
        video->type == VIR_DOMAIN_VIDEO_TYPE_VMVGA) {
        if (video->vram && video->vram < 1024) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           "%s", _("value for 'vram' must be at least "
                                   "1 MiB (1024 KiB)"));
            return -1;
        }
    }

    return 0;
}


static int
qemuDomainValidateStorageSource(virStorageSourcePtr src,
                                virQEMUCapsPtr qemuCaps)
{
    int actualType = virStorageSourceGetActualType(src);

    if (src->format == VIR_STORAGE_FILE_COW) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                      _("'cow' storage format is not supported"));
        return -1;
    }

    if (src->format == VIR_STORAGE_FILE_DIR) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("'directory' storage format is not directly supported by QEMU, "
                         "use 'dir' disk type instead"));
        return -1;
    }

    if (src->format == VIR_STORAGE_FILE_ISO) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("storage format 'iso' is not directly supported by QEMU, "
                         "use 'raw' instead"));
        return -1;
    }

    if (src->format == VIR_STORAGE_FILE_QCOW2 &&
        src->encryption &&
        src->encryption->format == VIR_STORAGE_ENCRYPTION_FORMAT_LUKS &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_QCOW2_LUKS)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("LUKS encrypted QCOW2 images are not suppored by this QEMU"));
        return -1;
    }

    if (src->format == VIR_STORAGE_FILE_FAT &&
        actualType != VIR_STORAGE_TYPE_DIR) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("storage format 'fat' is supported only with 'dir' "
                         "storage type"));
        return -1;
    }

    if (actualType == VIR_STORAGE_TYPE_DIR) {
        if (src->format > 0 &&
            src->format != VIR_STORAGE_FILE_FAT) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("storage type 'dir' requires use of storage format 'fat'"));
            return -1;
        }

        if (!src->readonly) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtual FAT storage can't be accessed in read-write mode"));
            return -1;
        }
    }

    return 0;
}


static int
qemuDomainDeviceDefValidateDisk(const virDomainDiskDef *disk,
                                virQEMUCapsPtr qemuCaps)
{
    const char *driverName = virDomainDiskGetDriver(disk);
    virStorageSourcePtr n;

    if (disk->src->shared && !disk->src->readonly) {
        if (disk->src->format <= VIR_STORAGE_FILE_AUTO) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shared access for disk '%s' requires use of "
                             "explicitly specified disk format"), disk->dst);
            return -1;
        }

        if (!qemuBlockStorageSourceSupportsConcurrentAccess(disk->src)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shared access for disk '%s' requires use of "
                             "supported storage format"), disk->dst);
            return -1;
        }
    }

    if (driverName && STRNEQ(driverName, "qemu")) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unsupported driver name '%s' for disk '%s'"),
                       driverName, disk->dst);
        return -1;
    }

    for (n = disk->src; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (qemuDomainValidateStorageSource(n, qemuCaps) < 0)
            return -1;
    }

    return 0;
}


static int
qemuDomainDeviceDefValidateControllerAttributes(const virDomainControllerDef *controller)
{
    if (!(controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI &&
          controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI)) {
        if (controller->queues) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("'queues' is only supported by virtio-scsi controller"));
            return -1;
        }
        if (controller->cmd_per_lun) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("'cmd_per_lun' is only supported by virtio-scsi controller"));
            return -1;
        }
        if (controller->max_sectors) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("'max_sectors' is only supported by virtio-scsi controller"));
            return -1;
        }
        if (controller->ioeventfd) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("'ioeventfd' is only supported by virtio-scsi controller"));
            return -1;
        }
        if (controller->iothread) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("'iothread' is only supported for virtio-scsi controller"));
            return -1;
        }
    }

    return 0;
}


/**
 * @qemuCaps: QEMU capabilities
 * @model: SCSI model to check
 *
 * Using the @qemuCaps, let's ensure the provided @model can be supported
 *
 * Returns true if acceptable, false otherwise with error message set.
 */
static bool
qemuDomainCheckSCSIControllerModel(virQEMUCapsPtr qemuCaps,
                                   int model)
{
    switch ((virDomainControllerModelSCSI) model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_LSI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support "
                             "the LSI 53C895A SCSI controller"));
            return false;
        }
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_SCSI)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support "
                             "virtio scsi controller"));
            return false;
        }
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
        /*TODO: need checking work here if necessary */
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1068:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_MPTSAS1068)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support "
                             "the LSI SAS1068 (MPT Fusion) controller"));
            return false;
        }
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_SCSI_MEGASAS)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This QEMU doesn't support "
                             "the LSI SAS1078 (MegaRAID) controller"));
            return false;
        }
        break;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AUTO:
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_BUSLOGIC:
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VMPVSCSI:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported controller model: %s"),
                       virDomainControllerModelSCSITypeToString(model));
        return false;
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected SCSI controller model %d"),
                       model);
        return false;
    }

    return true;
}


static int
qemuDomainDeviceDefValidateControllerIDE(const virDomainControllerDef *controller,
                                         const virDomainDef *def)
{
    /* first IDE controller is implicit on various machines */
    if (controller->idx == 0 && qemuDomainHasBuiltinIDE(def))
        return 0;

    /* Since we currently only support the integrated IDE
     * controller on various boards, if we ever get to here, it's
     * because some other machinetype had an IDE controller
     * specified, or one with a single IDE controller had multiple
     * IDE controllers specified.
     */
    if (qemuDomainHasBuiltinIDE(def))
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Only a single IDE controller is supported "
                         "for this machine type"));
    else
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("IDE controllers are unsupported for "
                         "this QEMU binary or machine type"));
    return -1;
}


/* qemuDomainCheckSCSIControllerIOThreads:
 * @controller: Pointer to controller def
 * @def: Pointer to domain def
 *
 * If this controller definition has iothreads set, let's make sure the
 * configuration is right before adding to the command line
 *
 * Returns true if either supported or there are no iothreads for controller;
 * otherwise, returns false if configuration is not quite right.
 */
static bool
qemuDomainCheckSCSIControllerIOThreads(const virDomainControllerDef *controller,
                                       const virDomainDef *def)
{
    if (!controller->iothread)
        return true;

    if (controller->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI &&
        controller->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
       virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("IOThreads only available for virtio pci and "
                         "virtio ccw controllers"));
       return false;
    }

    /* Can we find the controller iothread in the iothreadid list? */
    if (!virDomainIOThreadIDFind(def, controller->iothread)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("controller iothread '%u' not defined in iothreadid"),
                       controller->iothread);
        return false;
    }

    return true;
}


static int
qemuDomainDeviceDefValidateControllerSCSI(const virDomainControllerDef *controller,
                                          const virDomainDef *def)
{
    switch ((virDomainControllerModelSCSI) controller->model) {
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VIRTIO_SCSI:
            if (!qemuDomainCheckSCSIControllerIOThreads(controller, def))
                return -1;
            break;

        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_AUTO:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_BUSLOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1068:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_VMPVSCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_IBMVSCSI:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSISAS1078:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_DEFAULT:
        case VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LAST:
            break;
    }

    return 0;
}


/**
 * virDomainControllerPCIModelNameToQEMUCaps:
 * @modelName: model name
 *
 * Maps model names for PCI controllers (virDomainControllerPCIModelName)
 * to the QEMU capabilities required to use them (virQEMUCapsFlags).
 *
 * Returns: the QEMU capability itself (>0) on success; 0 if no QEMU
 *          capability is needed; <0 on error.
 */
static int
virDomainControllerPCIModelNameToQEMUCaps(int modelName)
{
    switch ((virDomainControllerPCIModelName) modelName) {
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCI_BRIDGE:
        return QEMU_CAPS_DEVICE_PCI_BRIDGE;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_I82801B11_BRIDGE:
        return QEMU_CAPS_DEVICE_DMI_TO_PCI_BRIDGE;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_IOH3420:
        return QEMU_CAPS_DEVICE_IOH3420;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_X3130_UPSTREAM:
        return QEMU_CAPS_DEVICE_X3130_UPSTREAM;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_XIO3130_DOWNSTREAM:
        return QEMU_CAPS_DEVICE_XIO3130_DOWNSTREAM;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PXB:
        return QEMU_CAPS_DEVICE_PXB;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PXB_PCIE:
        return QEMU_CAPS_DEVICE_PXB_PCIE;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCIE_ROOT_PORT:
        return QEMU_CAPS_DEVICE_PCIE_ROOT_PORT;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE:
        return QEMU_CAPS_DEVICE_SPAPR_PCI_HOST_BRIDGE;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCIE_PCI_BRIDGE:
        return QEMU_CAPS_DEVICE_PCIE_PCI_BRIDGE;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE:
        return 0;
    case VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_LAST:
    default:
        return -1;
    }

    return -1;
}


#define virReportControllerMissingOption(cont, model, modelName, option) \
    virReportError(VIR_ERR_INTERNAL_ERROR, \
                   _("Required option '%s' is not set for PCI controller " \
                     "with index '%d', model '%s' and modelName '%s'"), \
                   (option), (cont->idx), (model), (modelName));
#define virReportControllerInvalidOption(cont, model, modelName, option) \
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, \
                   _("Option '%s' is not valid for PCI controller " \
                     "with index '%d', model '%s' and modelName '%s'"), \
                   (option), (cont->idx), (model), (modelName));
#define virReportControllerInvalidValue(cont, model, modelName, option) \
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, \
                   _("Option '%s' has invalid value for PCI controller " \
                     "with index '%d', model '%s' and modelName '%s'"), \
                   (option), (cont->idx), (model), (modelName));


static int
qemuDomainDeviceDefValidateControllerPCI(const virDomainControllerDef *cont,
                                         const virDomainDef *def,
                                         virQEMUCapsPtr qemuCaps)

{
    const virDomainPCIControllerOpts *pciopts = &cont->opts.pciopts;
    const char *model = virDomainControllerModelPCITypeToString(cont->model);
    const char *modelName = virDomainControllerPCIModelNameTypeToString(pciopts->modelName);
    int cap = virDomainControllerPCIModelNameToQEMUCaps(pciopts->modelName);

    if (!model) {
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }
    if (!modelName) {
        virReportEnumRangeError(virDomainControllerPCIModelName, pciopts->modelName);
        return -1;
    }

    /* modelName */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        /* modelName should have been set automatically */
        if (pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE) {
            virReportControllerMissingOption(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        /* modelName must be set for pSeries guests, but it's an error
         * for it to be set for any other guest */
        if (qemuDomainIsPSeries(def)) {
            if (pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE) {
                virReportControllerMissingOption(cont, model, modelName, "modelName");
                return -1;
            }
        } else {
            if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE) {
                virReportControllerInvalidOption(cont, model, modelName, "modelName");
                return -1;
            }
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE) {
            virReportControllerInvalidOption(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* modelName (cont'd) */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE &&
            pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCI_BRIDGE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_I82801B11_BRIDGE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_IOH3420 &&
            pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCIE_ROOT_PORT) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_X3130_UPSTREAM) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_XIO3130_DOWNSTREAM) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PXB) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PXB_PCIE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_NONE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_PCIE_PCI_BRIDGE) {
            virReportControllerInvalidValue(cont, model, modelName, "modelName");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* index */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (cont->idx == 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Index for '%s' controllers must be > 0"),
                           model);
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        /* pSeries guests can have multiple PHBs, so it's expected that
         * the index will not be zero for some of them */
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT &&
            pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE) {
            break;
        }

        /* For all other pci-root and pcie-root controllers, though,
         * the index must be zero*/
        if (cont->idx != 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Index for '%s' controllers must be 0"),
                           model);
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* targetIndex */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        /* PHBs for pSeries guests must have been assigned a targetIndex */
        if (pciopts->targetIndex == -1 &&
            pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE) {
            virReportControllerMissingOption(cont, model, modelName, "targetIndex");
            return -1;
        }

        /* targetIndex only applies to PHBs, so for any other pci-root
         * controller it being present is an error */
        if (pciopts->targetIndex != -1 &&
            pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE) {
            virReportControllerInvalidOption(cont, model, modelName, "targetIndex");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->targetIndex != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "targetIndex");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* pcihole64 */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
        /* The pcihole64 option only applies to x86 guests */
        if ((pciopts->pcihole64 ||
             pciopts->pcihole64size != 0) &&
            !ARCH_IS_X86(def->os.arch)) {
            virReportControllerInvalidOption(cont, model, modelName, "pcihole64");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->pcihole64 ||
            pciopts->pcihole64size != 0) {
            virReportControllerInvalidOption(cont, model, modelName, "pcihole64");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* busNr */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
        if (pciopts->busNr == -1) {
            virReportControllerMissingOption(cont, model, modelName, "busNr");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->busNr != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "busNr");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* numaNode */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
        /* numaNode can be used for these controllers, but it's not set
         * automatically so it can be missing */
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
        /* Only PHBs support numaNode */
        if (pciopts->numaNode != -1 &&
            pciopts->modelName != VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE) {
            virReportControllerInvalidOption(cont, model, modelName, "numaNode");
            return -1;
        }

        /* However, the default PHB doesn't support numaNode */
        if (pciopts->numaNode != -1 &&
            pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE &&
            pciopts->targetIndex == 0) {
            virReportControllerInvalidOption(cont, model, modelName, "numaNode");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->numaNode != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "numaNode");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* chassisNr */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
        if (pciopts->chassisNr == -1) {
            virReportControllerMissingOption(cont, model, modelName, "chassisNr");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->chassisNr != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "chassisNr");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
        return -1;
    }

    /* chassis and port */
    switch ((virDomainControllerModelPCI) cont->model) {
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_DOWNSTREAM_PORT:
        if (pciopts->chassis == -1) {
            virReportControllerMissingOption(cont, model, modelName, "chassis");
            return -1;
        }
        if (pciopts->port == -1) {
            virReportControllerMissingOption(cont, model, modelName, "port");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_DMI_TO_PCI_BRIDGE:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_SWITCH_UPSTREAM_PORT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCIE_TO_PCI_BRIDGE:
        if (pciopts->chassis != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "chassis");
            return -1;
        }
        if (pciopts->port != -1) {
            virReportControllerInvalidOption(cont, model, modelName, "port");
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_DEFAULT:
    case VIR_DOMAIN_CONTROLLER_MODEL_PCI_LAST:
    default:
        virReportEnumRangeError(virDomainControllerModelPCI, cont->model);
    }

    /* QEMU device availability */
    if (cap < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown QEMU device for '%s' controller"),
                       modelName);
        return -1;
    }
    if (cap > 0 && !virQEMUCapsGet(qemuCaps, cap)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("The '%s' device is not supported by this QEMU binary"),
                       modelName);
        return -1;
    }

    /* PHBs didn't support numaNode from the very beginning, so an extra
     * capability check is required */
    if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT &&
        pciopts->modelName == VIR_DOMAIN_CONTROLLER_PCI_MODEL_NAME_SPAPR_PCI_HOST_BRIDGE &&
        pciopts->numaNode != -1 &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_SPAPR_PCI_HOST_BRIDGE_NUMA_NODE)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Option '%s' is not supported by '%s' device with this QEMU binary"),
                       "numaNode", modelName);
        return -1;
    }

    return 0;
}


#undef virReportControllerInvalidValue
#undef virReportControllerInvalidOption
#undef virReportControllerMissingOption


static int
qemuDomainDeviceDefValidateControllerSATA(const virDomainControllerDef *controller,
                                          const virDomainDef *def,
                                          virQEMUCapsPtr qemuCaps)
{
    /* first SATA controller on Q35 machines is implicit */
    if (controller->idx == 0 && qemuDomainIsQ35(def))
        return 0;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_ICH9_AHCI)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("SATA is not supported with this QEMU binary"));
        return -1;
    }
    return 0;
}


static int
qemuDomainDeviceDefValidateController(const virDomainControllerDef *controller,
                                      const virDomainDef *def,
                                      virQEMUCapsPtr qemuCaps)
{
    int ret = 0;

    if (!qemuDomainCheckCCWS390AddressSupport(def, controller->info, qemuCaps,
                                              "controller"))
        return -1;

    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI &&
        !qemuDomainCheckSCSIControllerModel(qemuCaps, controller->model))
        return -1;

    if (qemuDomainDeviceDefValidateControllerAttributes(controller) < 0)
        return -1;

    switch ((virDomainControllerType)controller->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
        ret = qemuDomainDeviceDefValidateControllerIDE(controller, def);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        ret = qemuDomainDeviceDefValidateControllerSCSI(controller, def);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_PCI:
        ret = qemuDomainDeviceDefValidateControllerPCI(controller, def,
                                                       qemuCaps);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
        ret = qemuDomainDeviceDefValidateControllerSATA(controller, def,
                                                        qemuCaps);
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
    case VIR_DOMAIN_CONTROLLER_TYPE_USB:
    case VIR_DOMAIN_CONTROLLER_TYPE_LAST:
        break;
    }

    return ret;
}


static int
qemuDomainDeviceDefValidateMemory(const virDomainMemoryDef *memory ATTRIBUTE_UNUSED,
                                  const virDomainDef *def)
{
    const long system_page_size = virGetSystemPageSizeKB();

    /* We can't guarantee any other mem.access
     * if no guest NUMA nodes are defined. */
    if (def->mem.nhugepages != 0 &&
        def->mem.hugepages[0].size != system_page_size &&
        virDomainNumaGetNodeCount(def->numa) == 0 &&
        def->mem.access != VIR_DOMAIN_MEMORY_ACCESS_DEFAULT &&
        def->mem.access != VIR_DOMAIN_MEMORY_ACCESS_PRIVATE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("memory access mode '%s' not supported "
                         "without guest numa node"),
                       virDomainMemoryAccessTypeToString(def->mem.access));
        return -1;
    }

    return 0;
}


static int
qemuDomainDeviceDefValidate(const virDomainDeviceDef *dev,
                            const virDomainDef *def,
                            void *opaque)
{
    int ret = 0;
    virQEMUDriverPtr driver = opaque;
    virQEMUCapsPtr qemuCaps = NULL;

    if (!(qemuCaps = virQEMUCapsCacheLookup(driver->qemuCapsCache,
                                            def->emulator)))
        return -1;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_NET:
        ret = qemuDomainDeviceDefValidateNetwork(dev->data.net);
        break;

    case VIR_DOMAIN_DEVICE_CHR:
        ret = qemuDomainChrDefValidate(dev->data.chr, def);
        break;

    case VIR_DOMAIN_DEVICE_SMARTCARD:
        ret = qemuDomainSmartcardDefValidate(dev->data.smartcard);
        break;

    case VIR_DOMAIN_DEVICE_RNG:
        ret = qemuDomainRNGDefValidate(dev->data.rng);
        break;

    case VIR_DOMAIN_DEVICE_REDIRDEV:
        ret = qemuDomainRedirdevDefValidate(dev->data.redirdev);
        break;

    case VIR_DOMAIN_DEVICE_WATCHDOG:
        ret = qemuDomainWatchdogDefValidate(dev->data.watchdog, def);
        break;

    case VIR_DOMAIN_DEVICE_HOSTDEV:
        ret = qemuDomainDeviceDefValidateHostdev(dev->data.hostdev, def);
        break;

    case VIR_DOMAIN_DEVICE_VIDEO:
        ret = qemuDomainDeviceDefValidateVideo(dev->data.video);
        break;

    case VIR_DOMAIN_DEVICE_DISK:
        ret = qemuDomainDeviceDefValidateDisk(dev->data.disk, qemuCaps);
        break;

    case VIR_DOMAIN_DEVICE_CONTROLLER:
        ret = qemuDomainDeviceDefValidateController(dev->data.controller, def,
                                                    qemuCaps);
        break;

    case VIR_DOMAIN_DEVICE_MEMORY:
        ret = qemuDomainDeviceDefValidateMemory(dev->data.memory, def);
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_LAST:
        break;
    }

    virObjectUnref(qemuCaps);
    return ret;
}


/**
 * qemuDomainDefaultNetModel:
 * @def: domain definition
 * @qemuCaps: qemu capabilities
 *
 * Returns the default network model for a given domain. Note that if @qemuCaps
 * is NULL this function may return NULL if the default model depends on the
 * capabilities.
 */
static const char *
qemuDomainDefaultNetModel(const virDomainDef *def,
                          virQEMUCapsPtr qemuCaps)
{
    if (ARCH_IS_S390(def->os.arch))
        return "virtio";

    if (def->os.arch == VIR_ARCH_ARMV7L ||
        def->os.arch == VIR_ARCH_AARCH64) {
        if (STREQ(def->os.machine, "versatilepb"))
            return "smc91c111";

        if (qemuDomainIsVirt(def))
            return "virtio";

        /* Incomplete. vexpress (and a few others) use this, but not all
         * arm boards */
        return "lan9118";
    }

    /* In all other cases the model depends on the capabilities. If they were
     * not provided don't report any default. */
    if (!qemuCaps)
        return NULL;

    /* Try several network devices in turn; each of these devices is
     * less likely be supported out-of-the-box by the guest operating
     * system than the previous one */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_RTL8139))
        return "rtl8139";
    else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_E1000))
        return "e1000";
    else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIRTIO_NET))
        return "virtio";

    /* We've had no luck detecting support for any network device,
     * but we have to return something: might as well be rtl8139 */
    return "rtl8139";
}


/*
 * Clear auto generated unix socket paths:
 *
 * libvirt 1.2.18 and older:
 *     {cfg->channelTargetDir}/{dom-name}.{target-name}
 *
 * libvirt 1.2.19 - 1.3.2:
 *     {cfg->channelTargetDir}/domain-{dom-name}/{target-name}
 *
 * libvirt 1.3.3 and newer:
 *     {cfg->channelTargetDir}/domain-{dom-id}-{short-dom-name}/{target-name}
 *
 * The unix socket path was stored in config XML until libvirt 1.3.0.
 * If someone specifies the same path as we generate, they shouldn't do it.
 *
 * This function clears the path for migration as well, so we need to clear
 * the path even if we are not storing it in the XML.
 */
static int
qemuDomainChrDefDropDefaultPath(virDomainChrDefPtr chr,
                                virQEMUDriverPtr driver)
{
    virQEMUDriverConfigPtr cfg;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *regexp = NULL;
    int ret = -1;

    if (chr->deviceType != VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL ||
        chr->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO ||
        chr->source->type != VIR_DOMAIN_CHR_TYPE_UNIX ||
        !chr->source->data.nix.path) {
        return 0;
    }

    cfg = virQEMUDriverGetConfig(driver);

    virBufferEscapeRegex(&buf, "^%s", cfg->channelTargetDir);
    virBufferAddLit(&buf, "/([^/]+\\.)|(domain-[^/]+/)");
    virBufferEscapeRegex(&buf, "%s$", chr->target.name);

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    regexp = virBufferContentAndReset(&buf);

    if (virStringMatch(chr->source->data.nix.path, regexp))
        VIR_FREE(chr->source->data.nix.path);

    ret = 0;
 cleanup:
    VIR_FREE(regexp);
    virObjectUnref(cfg);
    return ret;
}


static int
qemuDomainShmemDefPostParse(virDomainShmemDefPtr shm)
{
    /* This was the default since the introduction of this device. */
    if (shm->model != VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL && !shm->size)
        shm->size = 4 << 20;

    /* Nothing more to check/change for IVSHMEM */
    if (shm->model == VIR_DOMAIN_SHMEM_MODEL_IVSHMEM)
        return 0;

    if (!shm->server.enabled) {
        if (shm->model == VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_DOORBELL) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shmem model '%s' is supported "
                             "only with server option enabled"),
                           virDomainShmemModelTypeToString(shm->model));
            return -1;
        }

        if (shm->msi.enabled) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shmem model '%s' doesn't support "
                             "msi"),
                           virDomainShmemModelTypeToString(shm->model));
        }
    } else {
        if (shm->model == VIR_DOMAIN_SHMEM_MODEL_IVSHMEM_PLAIN) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shmem model '%s' is supported "
                             "only with server option disabled"),
                           virDomainShmemModelTypeToString(shm->model));
            return -1;
        }

        if (shm->size) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("shmem model '%s' does not support size setting"),
                           virDomainShmemModelTypeToString(shm->model));
            return -1;
        }
        shm->msi.enabled = true;
        if (!shm->msi.ioeventfd)
            shm->msi.ioeventfd = VIR_TRISTATE_SWITCH_ON;
    }

    return 0;
}


#define QEMU_USB_XHCI_MAXPORTS 15


static int
qemuDomainControllerDefPostParse(virDomainControllerDefPtr cont,
                                 const virDomainDef *def,
                                 virQEMUCapsPtr qemuCaps,
                                 unsigned int parseFlags)
{
    switch ((virDomainControllerType)cont->type) {
    case VIR_DOMAIN_CONTROLLER_TYPE_SCSI:
        /* Set the default SCSI controller model if not already set */
        if (qemuDomainSetSCSIControllerModel(def, cont, qemuCaps) < 0)
            return -1;
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_USB:
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT && qemuCaps) {
            /* Pick a suitable default model for the USB controller if none
             * has been selected by the user and we have the qemuCaps for
             * figuring out which contollers are supported.
             *
             * We rely on device availability instead of setting the model
             * unconditionally because, for some machine types, there's a
             * chance we will get away with using the legacy USB controller
             * when the relevant device is not available.
             *
             * See qemuBuildControllerDevCommandLine() */

            /* Default USB controller is piix3-uhci if available. */
            if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_PIIX3_USB_UHCI))
                cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI;

            if (ARCH_IS_S390(def->os.arch)) {
                if (cont->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
                    /* set the default USB model to none for s390 unless an
                     * address is found */
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_NONE;
                }
            } else if (ARCH_IS_PPC64(def->os.arch)) {
                /* To not break migration we need to set default USB controller
                 * for ppc64 to pci-ohci if we cannot change ABI of the VM.
                 * The nec-usb-xhci or qemu-xhci controller is used as default
                 * only for newly defined domains or devices. */
                if ((parseFlags & VIR_DOMAIN_DEF_PARSE_ABI_UPDATE) &&
                    virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QEMU_XHCI)) {
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI;
                } else if ((parseFlags & VIR_DOMAIN_DEF_PARSE_ABI_UPDATE) &&
                    virQEMUCapsGet(qemuCaps, QEMU_CAPS_NEC_USB_XHCI)) {
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI;
                } else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_PCI_OHCI)) {
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_PCI_OHCI;
                } else {
                    /* Explicitly fallback to legacy USB controller for PPC64. */
                    cont->model = -1;
                }
            } else if (def->os.arch == VIR_ARCH_AARCH64) {
                if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QEMU_XHCI))
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI;
                else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NEC_USB_XHCI))
                    cont->model = VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI;
            }
        }
        /* forbid usb model 'qusb1' and 'qusb2' in this kind of hyperviosr */
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_QUSB1 ||
            cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_QUSB2) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("USB controller model type 'qusb1' or 'qusb2' "
                             "is not supported in %s"),
                           virDomainVirtTypeToString(def->virtType));
            return -1;
        }
        if ((cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_NEC_XHCI ||
             cont->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_QEMU_XHCI) &&
            cont->opts.usbopts.ports > QEMU_USB_XHCI_MAXPORTS) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("'%s' controller only supports up to '%u' ports"),
                           virDomainControllerModelUSBTypeToString(cont->model),
                           QEMU_USB_XHCI_MAXPORTS);
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_PCI:

        /* pSeries guests can have multiple pci-root controllers,
         * but other machine types only support a single one */
        if (!qemuDomainIsPSeries(def) &&
            (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT ||
             cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT) &&
            cont->idx != 0) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("pci-root and pcie-root controllers "
                             "should have index 0"));
            return -1;
        }

        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_EXPANDER_BUS &&
            !qemuDomainIsI440FX(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("pci-expander-bus controllers are only supported "
                             "on 440fx-based machinetypes"));
            return -1;
        }
        if (cont->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_EXPANDER_BUS &&
            !qemuDomainIsQ35(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("pcie-expander-bus controllers are only supported "
                             "on q35-based machinetypes"));
            return -1;
        }

        /* if a PCI expander bus or pci-root on Pseries has a NUMA node
         * set, make sure that NUMA node is configured in the guest
         * <cpu><numa> array. NUMA cell id's in this array are numbered
         * from 0 .. size-1.
         */
        if (cont->opts.pciopts.numaNode >= 0 &&
            cont->opts.pciopts.numaNode >=
            (int)virDomainNumaGetNodeCount(def->numa)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s with index %d is "
                             "configured for a NUMA node (%d) "
                             "not present in the domain's "
                             "<cpu><numa> array (%zu)"),
                           virDomainControllerModelPCITypeToString(cont->model),
                           cont->idx, cont->opts.pciopts.numaNode,
                           virDomainNumaGetNodeCount(def->numa));
            return -1;
        }
        break;

    case VIR_DOMAIN_CONTROLLER_TYPE_SATA:
    case VIR_DOMAIN_CONTROLLER_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_CONTROLLER_TYPE_CCID:
    case VIR_DOMAIN_CONTROLLER_TYPE_IDE:
    case VIR_DOMAIN_CONTROLLER_TYPE_FDC:
    case VIR_DOMAIN_CONTROLLER_TYPE_LAST:
        break;
    }

    return 0;
}

static int
qemuDomainChrDefPostParse(virDomainChrDefPtr chr,
                          const virDomainDef *def,
                          virQEMUDriverPtr driver,
                          unsigned int parseFlags)
{
    /* Historically, isa-serial and the default matched, so in order to
     * maintain backwards compatibility we map them here. The actual default
     * will be picked below based on the architecture and machine type. */
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA) {
        chr->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE;
    }

    /* Set the default serial type */
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        chr->targetType == VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE) {
        if (ARCH_IS_X86(def->os.arch)) {
            chr->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA;
        } else if (qemuDomainIsPSeries(def)) {
            chr->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO;
        } else if (qemuDomainIsVirt(def)) {
            chr->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM;
        } else if (ARCH_IS_S390(def->os.arch)) {
            chr->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP;
        }
    }

    /* Set the default target model */
    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL &&
        chr->targetModel == VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE) {
        switch ((virDomainChrSerialTargetType)chr->targetType) {
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_USB_SERIAL;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PCI_SERIAL;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SPAPR_VTY;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_PL011;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP:
            chr->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_SCLPCONSOLE;
            break;
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE:
        case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_LAST:
            /* Nothing to do */
            break;
        }
    }

    /* clear auto generated unix socket path for inactive definitions */
    if (parseFlags & VIR_DOMAIN_DEF_PARSE_INACTIVE) {
        if (qemuDomainChrDefDropDefaultPath(chr, driver) < 0)
            return -1;

        /* For UNIX chardev if no path is provided we generate one.
         * This also implies that the mode is 'bind'. */
        if (chr->source &&
            chr->source->type == VIR_DOMAIN_CHR_TYPE_UNIX &&
            !chr->source->data.nix.path) {
            chr->source->data.nix.listen = true;
        }
    }

    return 0;
}

static int
qemuDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                             const virDomainDef *def,
                             virCapsPtr caps ATTRIBUTE_UNUSED,
                             unsigned int parseFlags,
                             void *opaque,
                             void *parseOpaque)
{
    virQEMUDriverPtr driver = opaque;
    /* Note that qemuCaps may be NULL when this function is called. This
     * function shall not fail in that case. It will be re-run on VM startup
     * with the capabilities populated. */
    virQEMUCapsPtr qemuCaps = parseOpaque;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (dev->type == VIR_DOMAIN_DEVICE_NET &&
        dev->data.net->type != VIR_DOMAIN_NET_TYPE_HOSTDEV &&
        !dev->data.net->model) {
        if (VIR_STRDUP(dev->data.net->model,
                       qemuDomainDefaultNetModel(def, qemuCaps)) < 0)
            goto cleanup;
    }

    /* set default disk types and drivers */
    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        virDomainDiskDefPtr disk = dev->data.disk;

        /* assign default storage format and driver according to config */
        if (cfg->allowDiskFormatProbing) {
            /* default disk format for drives */
            if (virDomainDiskGetFormat(disk) == VIR_STORAGE_FILE_NONE &&
                (virDomainDiskGetType(disk) == VIR_STORAGE_TYPE_FILE ||
                 virDomainDiskGetType(disk) == VIR_STORAGE_TYPE_BLOCK))
                virDomainDiskSetFormat(disk, VIR_STORAGE_FILE_AUTO);

            /* default disk format for mirrored drive */
            if (disk->mirror &&
                disk->mirror->format == VIR_STORAGE_FILE_NONE)
                disk->mirror->format = VIR_STORAGE_FILE_AUTO;
        } else {
            /* default driver if probing is forbidden */
            if (!virDomainDiskGetDriver(disk) &&
                virDomainDiskSetDriver(disk, "qemu") < 0)
                goto cleanup;

            /* default disk format for drives */
            if (virDomainDiskGetFormat(disk) == VIR_STORAGE_FILE_NONE &&
                (virDomainDiskGetType(disk) == VIR_STORAGE_TYPE_FILE ||
                 virDomainDiskGetType(disk) == VIR_STORAGE_TYPE_BLOCK))
                virDomainDiskSetFormat(disk, VIR_STORAGE_FILE_RAW);

            /* default disk format for mirrored drive */
            if (disk->mirror &&
                disk->mirror->format == VIR_STORAGE_FILE_NONE)
                disk->mirror->format = VIR_STORAGE_FILE_RAW;
        }
    }

    if (dev->type == VIR_DOMAIN_DEVICE_VIDEO) {
        if (dev->data.video->type == VIR_DOMAIN_VIDEO_TYPE_DEFAULT) {
            if (ARCH_IS_PPC64(def->os.arch))
                dev->data.video->type = VIR_DOMAIN_VIDEO_TYPE_VGA;
            else if (qemuDomainIsVirt(def) || ARCH_IS_S390(def->os.arch))
                dev->data.video->type = VIR_DOMAIN_VIDEO_TYPE_VIRTIO;
            else
                dev->data.video->type = VIR_DOMAIN_VIDEO_TYPE_CIRRUS;
        }

        if (dev->data.video->type == VIR_DOMAIN_VIDEO_TYPE_QXL &&
            !dev->data.video->vgamem) {
            dev->data.video->vgamem = QEMU_QXL_VGAMEM_DEFAULT;
        }
    }

    if (dev->type == VIR_DOMAIN_DEVICE_PANIC &&
        dev->data.panic->model == VIR_DOMAIN_PANIC_MODEL_DEFAULT) {
        if (qemuDomainIsPSeries(def))
            dev->data.panic->model = VIR_DOMAIN_PANIC_MODEL_PSERIES;
        else if (ARCH_IS_S390(def->os.arch))
            dev->data.panic->model = VIR_DOMAIN_PANIC_MODEL_S390;
        else
            dev->data.panic->model = VIR_DOMAIN_PANIC_MODEL_ISA;
    }

    if (dev->type == VIR_DOMAIN_DEVICE_CONTROLLER &&
        qemuDomainControllerDefPostParse(dev->data.controller, def,
                                         qemuCaps, parseFlags) < 0)
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_SHMEM &&
        qemuDomainShmemDefPostParse(dev->data.shmem) < 0)
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_CHR &&
        qemuDomainChrDefPostParse(dev->data.chr, def, driver, parseFlags) < 0) {
        goto cleanup;
    }

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


static int
qemuDomainDefAssignAddresses(virDomainDef *def,
                             virCapsPtr caps ATTRIBUTE_UNUSED,
                             unsigned int parseFlags ATTRIBUTE_UNUSED,
                             void *opaque,
                             void *parseOpaque)
{
    virQEMUDriverPtr driver = opaque;
    /* Note that qemuCaps may be NULL when this function is called. This
     * function shall not fail in that case. It will be re-run on VM startup
     * with the capabilities populated. */
    virQEMUCapsPtr qemuCaps = parseOpaque;
    bool newDomain = parseFlags & VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;

    /* Skip address assignment if @qemuCaps is not present. In such case devices
     * which are automatically added may be missing. Additionally @qemuCaps should
     * only be missing when reloading configs, thus addresses were already
     * assigned. */
    if (!qemuCaps)
        return 1;

    return qemuDomainAssignAddresses(def, qemuCaps, driver, NULL, newDomain);
}


static int
qemuDomainPostParseDataAlloc(const virDomainDef *def,
                             virCapsPtr caps ATTRIBUTE_UNUSED,
                             unsigned int parseFlags ATTRIBUTE_UNUSED,
                             void *opaque,
                             void **parseOpaque)
{
    virQEMUDriverPtr driver = opaque;

    if (!(*parseOpaque = virQEMUCapsCacheLookup(driver->qemuCapsCache,
                                                def->emulator)))
        return 1;

    return 0;
}


static void
qemuDomainPostParseDataFree(void *parseOpaque)
{
    virQEMUCapsPtr qemuCaps = parseOpaque;

    virObjectUnref(qemuCaps);
}


virDomainDefParserConfig virQEMUDriverDomainDefParserConfig = {
    .domainPostParseBasicCallback = qemuDomainDefPostParseBasic,
    .domainPostParseDataAlloc = qemuDomainPostParseDataAlloc,
    .domainPostParseDataFree = qemuDomainPostParseDataFree,
    .devicesPostParseCallback = qemuDomainDeviceDefPostParse,
    .domainPostParseCallback = qemuDomainDefPostParse,
    .assignAddressesCallback = qemuDomainDefAssignAddresses,
    .domainValidateCallback = qemuDomainDefValidate,
    .deviceValidateCallback = qemuDomainDeviceDefValidate,

    .features = VIR_DOMAIN_DEF_FEATURE_MEMORY_HOTPLUG |
                VIR_DOMAIN_DEF_FEATURE_OFFLINE_VCPUPIN |
                VIR_DOMAIN_DEF_FEATURE_INDIVIDUAL_VCPUS |
                VIR_DOMAIN_DEF_FEATURE_USER_ALIAS,
};


static void
qemuDomainObjSaveJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (virDomainObjIsActive(obj)) {
        if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, obj, driver->caps) < 0)
            VIR_WARN("Failed to save status on vm %s", obj->def->name);
    }

    virObjectUnref(cfg);
}

void
qemuDomainObjSetJobPhase(virQEMUDriverPtr driver,
                         virDomainObjPtr obj,
                         int phase)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    unsigned long long me = virThreadSelfID();

    if (!priv->job.asyncJob)
        return;

    VIR_DEBUG("Setting '%s' phase to '%s'",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              qemuDomainAsyncJobPhaseToString(priv->job.asyncJob, phase));

    if (priv->job.asyncOwner && me != priv->job.asyncOwner) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
                 priv->job.asyncOwner);
    }

    priv->job.phase = phase;
    priv->job.asyncOwner = me;
    qemuDomainObjSaveJob(driver, obj);
}

void
qemuDomainObjSetAsyncJobMask(virDomainObjPtr obj,
                             unsigned long long allowedJobs)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (!priv->job.asyncJob)
        return;

    priv->job.mask = allowedJobs | JOB_MASK(QEMU_JOB_DESTROY);
}

void
qemuDomainObjDiscardAsyncJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (priv->job.active == QEMU_JOB_ASYNC_NESTED)
        qemuDomainObjResetJob(priv);
    qemuDomainObjResetAsyncJob(priv);
    qemuDomainObjSaveJob(driver, obj);
}

void
qemuDomainObjReleaseAsyncJob(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    VIR_DEBUG("Releasing ownership of '%s' async job",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob));

    if (priv->job.asyncOwner != virThreadSelfID()) {
        VIR_WARN("'%s' async job is owned by thread %llu",
                 qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
                 priv->job.asyncOwner);
    }
    priv->job.asyncOwner = 0;
}

static bool
qemuDomainNestedJobAllowed(qemuDomainObjPrivatePtr priv, qemuDomainJob job)
{
    return !priv->job.asyncJob || (priv->job.mask & JOB_MASK(job)) != 0;
}

bool
qemuDomainJobAllowed(qemuDomainObjPrivatePtr priv, qemuDomainJob job)
{
    return !priv->job.active && qemuDomainNestedJobAllowed(priv, job);
}

/* Give up waiting for mutex after 30 seconds */
#define QEMU_JOB_WAIT_TIME (1000ull * 30)

/*
 * obj must be locked before calling
 */
static int ATTRIBUTE_NONNULL(1)
qemuDomainObjBeginJobInternal(virQEMUDriverPtr driver,
                              virDomainObjPtr obj,
                              qemuDomainJob job,
                              qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    unsigned long long now;
    unsigned long long then;
    bool nested = job == QEMU_JOB_ASYNC_NESTED;
    bool async = job == QEMU_JOB_ASYNC;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    const char *blocker = NULL;
    int ret = -1;
    unsigned long long duration = 0;
    unsigned long long asyncDuration = 0;
    const char *jobStr;

    if (async)
        jobStr = qemuDomainAsyncJobTypeToString(asyncJob);
    else
        jobStr = qemuDomainJobTypeToString(job);

    VIR_DEBUG("Starting %s: %s (vm=%p name=%s, current job=%s async=%s)",
              async ? "async job" : "job", jobStr, obj, obj->def->name,
              qemuDomainJobTypeToString(priv->job.active),
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob));

    if (virTimeMillisNow(&now) < 0) {
        virObjectUnref(cfg);
        return -1;
    }

    priv->jobs_queued++;
    then = now + QEMU_JOB_WAIT_TIME;

 retry:
    if ((!async && job != QEMU_JOB_DESTROY) &&
        cfg->maxQueuedJobs &&
        priv->jobs_queued > cfg->maxQueuedJobs) {
        goto error;
    }

    while (!nested && !qemuDomainNestedJobAllowed(priv, job)) {
        VIR_DEBUG("Waiting for async job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&priv->job.asyncCond, &obj->parent.lock, then) < 0)
            goto error;
    }

    while (priv->job.active) {
        VIR_DEBUG("Waiting for job (vm=%p name=%s)", obj, obj->def->name);
        if (virCondWaitUntil(&priv->job.cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    /* No job is active but a new async job could have been started while obj
     * was unlocked, so we need to recheck it. */
    if (!nested && !qemuDomainNestedJobAllowed(priv, job))
        goto retry;

    qemuDomainObjResetJob(priv);

    ignore_value(virTimeMillisNow(&now));

    if (job != QEMU_JOB_ASYNC) {
        VIR_DEBUG("Started job: %s (async=%s vm=%p name=%s)",
                   qemuDomainJobTypeToString(job),
                  qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
                  obj, obj->def->name);
        priv->job.active = job;
        priv->job.owner = virThreadSelfID();
        priv->job.ownerAPI = virThreadJobGet();
        priv->job.started = now;
    } else {
        VIR_DEBUG("Started async job: %s (vm=%p name=%s)",
                  qemuDomainAsyncJobTypeToString(asyncJob),
                  obj, obj->def->name);
        qemuDomainObjResetAsyncJob(priv);
        if (VIR_ALLOC(priv->job.current) < 0)
            goto cleanup;
        priv->job.current->status = QEMU_DOMAIN_JOB_STATUS_ACTIVE;
        priv->job.asyncJob = asyncJob;
        priv->job.asyncOwner = virThreadSelfID();
        priv->job.asyncOwnerAPI = virThreadJobGet();
        priv->job.asyncStarted = now;
        priv->job.current->started = now;
    }

    if (qemuDomainTrackJob(job))
        qemuDomainObjSaveJob(driver, obj);

    virObjectUnref(cfg);
    return 0;

 error:
    ignore_value(virTimeMillisNow(&now));
    if (priv->job.active && priv->job.started)
        duration = now - priv->job.started;
    if (priv->job.asyncJob && priv->job.asyncStarted)
        asyncDuration = now - priv->job.asyncStarted;

    VIR_WARN("Cannot start job (%s, %s) for domain %s; "
             "current job is (%s, %s) "
             "owned by (%llu %s, %llu %s (flags=0x%lx)) "
             "for (%llus, %llus)",
             qemuDomainJobTypeToString(job),
             qemuDomainAsyncJobTypeToString(asyncJob),
             obj->def->name,
             qemuDomainJobTypeToString(priv->job.active),
             qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
             priv->job.owner, NULLSTR(priv->job.ownerAPI),
             priv->job.asyncOwner, NULLSTR(priv->job.asyncOwnerAPI),
             priv->job.apiFlags,
             duration / 1000, asyncDuration / 1000);

    if (nested || qemuDomainNestedJobAllowed(priv, job))
        blocker = priv->job.ownerAPI;
    else
        blocker = priv->job.asyncOwnerAPI;

    ret = -1;
    if (errno == ETIMEDOUT) {
        if (blocker) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("cannot acquire state change lock (held by %s)"),
                           blocker);
        } else {
            virReportError(VIR_ERR_OPERATION_TIMEOUT, "%s",
                           _("cannot acquire state change lock"));
        }
        ret = -2;
    } else if (cfg->maxQueuedJobs &&
               priv->jobs_queued > cfg->maxQueuedJobs) {
        if (blocker) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("cannot acquire state change lock (held by %s) "
                             "due to max_queued limit"),
                           blocker);
        } else {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("cannot acquire state change lock "
                             "due to max_queued limit"));
        }
        ret = -2;
    } else {
        virReportSystemError(errno, "%s", _("cannot acquire job mutex"));
    }

 cleanup:
    priv->jobs_queued--;
    virObjectUnref(cfg);
    return ret;
}

/*
 * obj must be locked before calling
 *
 * This must be called by anything that will change the VM state
 * in any way, or anything that will use the QEMU monitor.
 *
 * Successful calls must be followed by EndJob eventually
 */
int qemuDomainObjBeginJob(virQEMUDriverPtr driver,
                          virDomainObjPtr obj,
                          qemuDomainJob job)
{
    if (qemuDomainObjBeginJobInternal(driver, obj, job,
                                      QEMU_ASYNC_JOB_NONE) < 0)
        return -1;
    else
        return 0;
}

int qemuDomainObjBeginAsyncJob(virQEMUDriverPtr driver,
                               virDomainObjPtr obj,
                               qemuDomainAsyncJob asyncJob,
                               virDomainJobOperation operation,
                               unsigned long apiFlags)
{
    qemuDomainObjPrivatePtr priv;

    if (qemuDomainObjBeginJobInternal(driver, obj, QEMU_JOB_ASYNC,
                                      asyncJob) < 0)
        return -1;

    priv = obj->privateData;
    priv->job.current->operation = operation;
    priv->job.apiFlags = apiFlags;
    return 0;
}

int
qemuDomainObjBeginNestedJob(virQEMUDriverPtr driver,
                            virDomainObjPtr obj,
                            qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (asyncJob != priv->job.asyncJob) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected async job %d"), asyncJob);
        return -1;
    }

    if (priv->job.asyncOwner != virThreadSelfID()) {
        VIR_WARN("This thread doesn't seem to be the async job owner: %llu",
                 priv->job.asyncOwner);
    }

    return qemuDomainObjBeginJobInternal(driver, obj,
                                         QEMU_JOB_ASYNC_NESTED,
                                         QEMU_ASYNC_JOB_NONE);
}


/*
 * obj must be locked and have a reference before calling
 *
 * To be called after completing the work associated with the
 * earlier qemuDomainBeginJob() call
 */
void
qemuDomainObjEndJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuDomainJob job = priv->job.active;

    priv->jobs_queued--;

    VIR_DEBUG("Stopping job: %s (async=%s vm=%p name=%s)",
              qemuDomainJobTypeToString(job),
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    qemuDomainObjResetJob(priv);
    if (qemuDomainTrackJob(job))
        qemuDomainObjSaveJob(driver, obj);
    virCondSignal(&priv->job.cond);
}

void
qemuDomainObjEndAsyncJob(virQEMUDriverPtr driver, virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    priv->jobs_queued--;

    VIR_DEBUG("Stopping async job: %s (vm=%p name=%s)",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    qemuDomainObjResetAsyncJob(priv);
    qemuDomainObjSaveJob(driver, obj);
    virCondBroadcast(&priv->job.asyncCond);
}

void
qemuDomainObjAbortAsyncJob(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    VIR_DEBUG("Requesting abort of async job: %s (vm=%p name=%s)",
              qemuDomainAsyncJobTypeToString(priv->job.asyncJob),
              obj, obj->def->name);

    priv->job.abortJob = true;
    virDomainObjBroadcast(obj);
}

/*
 * obj must be locked before calling
 *
 * To be called immediately before any QEMU monitor API call
 * Must have already either called qemuDomainObjBeginJob() and checked
 * that the VM is still active; may not be used for nested async jobs.
 *
 * To be followed with qemuDomainObjExitMonitor() once complete
 */
static int
qemuDomainObjEnterMonitorInternal(virQEMUDriverPtr driver,
                                  virDomainObjPtr obj,
                                  qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (asyncJob != QEMU_ASYNC_JOB_NONE) {
        int ret;
        if ((ret = qemuDomainObjBeginNestedJob(driver, obj, asyncJob)) < 0)
            return ret;
        if (!virDomainObjIsActive(obj)) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("domain is no longer running"));
            qemuDomainObjEndJob(driver, obj);
            return -1;
        }
    } else if (priv->job.asyncOwner == virThreadSelfID()) {
        VIR_WARN("This thread seems to be the async job owner; entering"
                 " monitor without asking for a nested job is dangerous");
    }

    VIR_DEBUG("Entering monitor (mon=%p vm=%p name=%s)",
              priv->mon, obj, obj->def->name);
    virObjectLock(priv->mon);
    virObjectRef(priv->mon);
    ignore_value(virTimeMillisNow(&priv->monStart));
    virObjectUnlock(obj);

    return 0;
}

static void ATTRIBUTE_NONNULL(1)
qemuDomainObjExitMonitorInternal(virQEMUDriverPtr driver,
                                 virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    bool hasRefs;

    hasRefs = virObjectUnref(priv->mon);

    if (hasRefs)
        virObjectUnlock(priv->mon);

    virObjectLock(obj);
    VIR_DEBUG("Exited monitor (mon=%p vm=%p name=%s)",
              priv->mon, obj, obj->def->name);

    priv->monStart = 0;
    if (!hasRefs)
        priv->mon = NULL;

    if (priv->job.active == QEMU_JOB_ASYNC_NESTED)
        qemuDomainObjEndJob(driver, obj);
}

void qemuDomainObjEnterMonitor(virQEMUDriverPtr driver,
                               virDomainObjPtr obj)
{
    ignore_value(qemuDomainObjEnterMonitorInternal(driver, obj,
                                                   QEMU_ASYNC_JOB_NONE));
}

/* obj must NOT be locked before calling
 *
 * Should be paired with an earlier qemuDomainObjEnterMonitor() call
 *
 * Returns -1 if the domain is no longer alive after exiting the monitor.
 * In that case, the caller should be careful when using obj's data,
 * e.g. the live definition in vm->def has been freed by qemuProcessStop
 * and replaced by the persistent definition, so pointers stolen
 * from the live definition could no longer be valid.
 */
int qemuDomainObjExitMonitor(virQEMUDriverPtr driver,
                             virDomainObjPtr obj)
{
    qemuDomainObjExitMonitorInternal(driver, obj);
    if (!virDomainObjIsActive(obj)) {
        if (!virGetLastError())
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("domain is no longer running"));
        return -1;
    }
    return 0;
}

/*
 * obj must be locked before calling
 *
 * To be called immediately before any QEMU monitor API call.
 * Must have already either called qemuDomainObjBeginJob()
 * and checked that the VM is still active, with asyncJob of
 * QEMU_ASYNC_JOB_NONE; or already called qemuDomainObjBeginAsyncJob,
 * with the same asyncJob.
 *
 * Returns 0 if job was started, in which case this must be followed with
 * qemuDomainObjExitMonitor(); -2 if waiting for the nested job times out;
 * or -1 if the job could not be started (probably because the vm exited
 * in the meantime).
 */
int
qemuDomainObjEnterMonitorAsync(virQEMUDriverPtr driver,
                               virDomainObjPtr obj,
                               qemuDomainAsyncJob asyncJob)
{
    return qemuDomainObjEnterMonitorInternal(driver, obj, asyncJob);
}


/*
 * obj must be locked before calling
 *
 * To be called immediately before any QEMU agent API call.
 * Must have already called qemuDomainObjBeginJob() and checked
 * that the VM is still active.
 *
 * To be followed with qemuDomainObjExitAgent() once complete
 */
qemuAgentPtr
qemuDomainObjEnterAgent(virDomainObjPtr obj)
{
    qemuDomainObjPrivatePtr priv = obj->privateData;
    qemuAgentPtr agent = priv->agent;

    VIR_DEBUG("Entering agent (agent=%p vm=%p name=%s)",
              priv->agent, obj, obj->def->name);

    virObjectLock(agent);
    virObjectRef(agent);
    virObjectUnlock(obj);

    return agent;
}


/* obj must NOT be locked before calling
 *
 * Should be paired with an earlier qemuDomainObjEnterAgent() call
 */
void
qemuDomainObjExitAgent(virDomainObjPtr obj, qemuAgentPtr agent)
{
    virObjectUnlock(agent);
    virObjectUnref(agent);
    virObjectLock(obj);

    VIR_DEBUG("Exited agent (agent=%p vm=%p name=%s)",
              agent, obj, obj->def->name);
}

void qemuDomainObjEnterRemote(virDomainObjPtr obj)
{
    VIR_DEBUG("Entering remote (vm=%p name=%s)",
              obj, obj->def->name);
    virObjectUnlock(obj);
}

void qemuDomainObjExitRemote(virDomainObjPtr obj)
{
    virObjectLock(obj);
    VIR_DEBUG("Exited remote (vm=%p name=%s)",
              obj, obj->def->name);
}


static virDomainDefPtr
qemuDomainDefFromXML(virQEMUDriverPtr driver,
                     const char *xml)
{
    virCapsPtr caps;
    virDomainDefPtr def;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        return NULL;

    def = virDomainDefParseString(xml, caps, driver->xmlopt, NULL,
                                  VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                  VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE);

    virObjectUnref(caps);
    return def;
}


virDomainDefPtr
qemuDomainDefCopy(virQEMUDriverPtr driver,
                  virDomainDefPtr src,
                  unsigned int flags)
{
    virDomainDefPtr ret = NULL;
    char *xml;

    if (!(xml = qemuDomainDefFormatXML(driver, src, flags)))
        return NULL;

    ret = qemuDomainDefFromXML(driver, xml);

    VIR_FREE(xml);
    return ret;
}


static int
qemuDomainDefFormatBufInternal(virQEMUDriverPtr driver,
                               virDomainDefPtr def,
                               virCPUDefPtr origCPU,
                               unsigned int flags,
                               virBuffer *buf)
{
    int ret = -1;
    virDomainDefPtr copy = NULL;
    virCapsPtr caps = NULL;
    virQEMUCapsPtr qemuCaps = NULL;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        goto cleanup;

    if (!(flags & (VIR_DOMAIN_XML_UPDATE_CPU | VIR_DOMAIN_XML_MIGRATABLE)))
        goto format;

    if (!(copy = virDomainDefCopy(def, caps, driver->xmlopt, NULL,
                                  flags & VIR_DOMAIN_XML_MIGRATABLE)))
        goto cleanup;

    def = copy;

    /* Update guest CPU requirements according to host CPU */
    if ((flags & VIR_DOMAIN_XML_UPDATE_CPU) &&
        def->cpu &&
        (def->cpu->mode != VIR_CPU_MODE_CUSTOM ||
         def->cpu->model)) {
        if (!(qemuCaps = virQEMUCapsCacheLookupCopy(driver->qemuCapsCache,
                                                    def->emulator,
                                                    def->os.machine)))
            goto cleanup;

        if (virCPUUpdate(def->os.arch, def->cpu,
                         virQEMUCapsGetHostModel(qemuCaps, def->virtType,
                                                 VIR_QEMU_CAPS_HOST_CPU_MIGRATABLE)) < 0)
            goto cleanup;
    }

    if ((flags & VIR_DOMAIN_XML_MIGRATABLE)) {
        size_t i;
        int toremove = 0;
        virDomainControllerDefPtr usb = NULL, pci = NULL;

        /* If only the default USB controller is present, we can remove it
         * and make the XML compatible with older versions of libvirt which
         * didn't support USB controllers in the XML but always added the
         * default one to qemu anyway.
         */
        for (i = 0; i < def->ncontrollers; i++) {
            if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) {
                if (usb) {
                    usb = NULL;
                    break;
                }
                usb = def->controllers[i];
            }
        }

        /* In order to maintain compatibility with version of libvirt that
         * didn't support <controller type='usb'/> (<= 0.9.4), we need to
         * drop the default USB controller, ie. a USB controller at index
         * zero with no model or with the default piix3-ohci model.
         *
         * However, we only need to do so for x86 i440fx machine types,
         * because other architectures and machine types were introduced
         * when libvirt already supported <controller type='usb'/>.
         */
        if (ARCH_IS_X86(def->os.arch) && qemuDomainIsI440FX(def) &&
            usb && usb->idx == 0 &&
            (usb->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_DEFAULT ||
             usb->model == VIR_DOMAIN_CONTROLLER_MODEL_USB_PIIX3_UHCI) &&
            !virDomainDeviceAliasIsUserAlias(usb->info.alias)) {
            VIR_DEBUG("Removing default USB controller from domain '%s'"
                      " for migration compatibility", def->name);
            toremove++;
        } else {
            usb = NULL;
        }

        /* Remove the default PCI controller if there is only one present
         * and its model is pci-root */
        for (i = 0; i < def->ncontrollers; i++) {
            if (def->controllers[i]->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI) {
                if (pci) {
                    pci = NULL;
                    break;
                }
                pci = def->controllers[i];
            }
        }

        if (pci && pci->idx == 0 &&
            pci->model == VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT &&
            !virDomainDeviceAliasIsUserAlias(pci->info.alias) &&
            !pci->opts.pciopts.pcihole64) {
            VIR_DEBUG("Removing default pci-root from domain '%s'"
                      " for migration compatibility", def->name);
            toremove++;
        } else {
            pci = NULL;
        }

        if (toremove) {
            virDomainControllerDefPtr *controllers = def->controllers;
            int ncontrollers = def->ncontrollers;

            if (VIR_ALLOC_N(def->controllers, ncontrollers - toremove) < 0) {
                def->controllers = controllers;
                goto cleanup;
            }

            def->ncontrollers = 0;
            for (i = 0; i < ncontrollers; i++) {
                if (controllers[i] != usb && controllers[i] != pci)
                    def->controllers[def->ncontrollers++] = controllers[i];
            }

            VIR_FREE(controllers);
            virDomainControllerDefFree(pci);
            virDomainControllerDefFree(usb);
        }

        /* Remove the panic device for selected models if present */
        for (i = 0; i < def->npanics; i++) {
            if (def->panics[i]->model == VIR_DOMAIN_PANIC_MODEL_S390 ||
                def->panics[i]->model == VIR_DOMAIN_PANIC_MODEL_PSERIES) {
                VIR_DELETE_ELEMENT(def->panics, i, def->npanics);
                break;
            }
        }

        for (i = 0; i < def->nchannels; i++) {
            if (qemuDomainChrDefDropDefaultPath(def->channels[i], driver) < 0)
                goto cleanup;
        }

        for (i = 0; i < def->nserials; i++) {
            virDomainChrDefPtr serial = def->serials[i];

            /* Historically, the native console type for some machine types
             * was not set at all, which means it defaulted to ISA even
             * though that was not even remotely accurate. To ensure migration
             * towards older libvirt versions works for such guests, we switch
             * it back to the default here */
            if (flags & VIR_DOMAIN_XML_MIGRATABLE) {
                switch ((virDomainChrSerialTargetType)serial->targetType) {
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SPAPR_VIO:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SYSTEM:
                    serial->targetType = VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE;
                    serial->targetModel = VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_NONE;
                    break;
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_ISA:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_PCI:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_USB:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_SCLP:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_NONE:
                case VIR_DOMAIN_CHR_SERIAL_TARGET_TYPE_LAST:
                    /* Nothing to do */
                    break;
                }
            }
        }

        /* Replace the CPU definition updated according to QEMU with the one
         * used for starting the domain. The updated def will be sent
         * separately for backward compatibility.
         */
        if (origCPU) {
            virCPUDefFree(def->cpu);
            if (!(def->cpu = virCPUDefCopy(origCPU)))
                goto cleanup;
        }
    }

 format:
    ret = virDomainDefFormatInternal(def, caps,
                                     virDomainDefFormatConvertXMLFlags(flags),
                                     buf, driver->xmlopt);

 cleanup:
    virDomainDefFree(copy);
    virObjectUnref(caps);
    virObjectUnref(qemuCaps);
    return ret;
}


int
qemuDomainDefFormatBuf(virQEMUDriverPtr driver,
                       virDomainDefPtr def,
                       unsigned int flags,
                       virBufferPtr buf)
{
    return qemuDomainDefFormatBufInternal(driver, def, NULL, flags, buf);
}


static char *
qemuDomainDefFormatXMLInternal(virQEMUDriverPtr driver,
                               virDomainDefPtr def,
                               virCPUDefPtr origCPU,
                               unsigned int flags)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (qemuDomainDefFormatBufInternal(driver, def, origCPU, flags, &buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


char *
qemuDomainDefFormatXML(virQEMUDriverPtr driver,
                       virDomainDefPtr def,
                       unsigned int flags)
{
    return qemuDomainDefFormatXMLInternal(driver, def, NULL, flags);
}


char *qemuDomainFormatXML(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          unsigned int flags)
{
    virDomainDefPtr def;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCPUDefPtr origCPU = NULL;

    if ((flags & VIR_DOMAIN_XML_INACTIVE) && vm->newDef) {
        def = vm->newDef;
    } else {
        def = vm->def;
        origCPU = priv->origCPU;
    }

    return qemuDomainDefFormatXMLInternal(driver, def, origCPU, flags);
}

char *
qemuDomainDefFormatLive(virQEMUDriverPtr driver,
                        virDomainDefPtr def,
                        virCPUDefPtr origCPU,
                        bool inactive,
                        bool compatible)
{
    unsigned int flags = QEMU_DOMAIN_FORMAT_LIVE_FLAGS;

    if (inactive)
        flags |= VIR_DOMAIN_XML_INACTIVE;
    if (compatible)
        flags |= VIR_DOMAIN_XML_MIGRATABLE;

    return qemuDomainDefFormatXMLInternal(driver, def, origCPU, flags);
}


/* qemuDomainFilePathIsHostCDROM
 * @path: Supplied path.
 *
 * Determine if the path is a host CD-ROM path. Typically this is
 * either /dev/cdrom[n] or /dev/srN, so those are easy checks, but
 * it's also possible that @path resolves to /dev/srN, so check for
 * those conditions on @path in order to emit the tainted message.
 *
 * Returns true if the path is a CDROM, false otherwise or on error.
 */
static bool
qemuDomainFilePathIsHostCDROM(const char *path)
{
    bool ret = false;
    char *linkpath = NULL;

    if (virFileResolveLink(path, &linkpath) < 0)
        goto cleanup;

    if (STRPREFIX(path, "/dev/cdrom") || STRPREFIX(path, "/dev/sr") ||
        STRPREFIX(linkpath, "/dev/sr"))
        ret = true;

 cleanup:
    VIR_FREE(linkpath);
    return ret;
}


void qemuDomainObjTaint(virQEMUDriverPtr driver,
                        virDomainObjPtr obj,
                        virDomainTaintFlags taint,
                        qemuDomainLogContextPtr logCtxt)
{
    virErrorPtr orig_err = NULL;
    bool closeLog = false;
    char *timestamp = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!virDomainObjTaint(obj, taint))
        return;

    virUUIDFormat(obj->def->uuid, uuidstr);

    VIR_WARN("Domain id=%d name='%s' uuid=%s is tainted: %s",
             obj->def->id,
             obj->def->name,
             uuidstr,
             virDomainTaintTypeToString(taint));

    /* We don't care about errors logging taint info, so
     * preserve original error, and clear any error that
     * is raised */
    orig_err = virSaveLastError();

    if (!(timestamp = virTimeStringNow()))
        goto cleanup;

    if (logCtxt == NULL) {
        logCtxt = qemuDomainLogContextNew(driver, obj,
                                          QEMU_DOMAIN_LOG_CONTEXT_MODE_ATTACH);
        if (!logCtxt) {
            VIR_WARN("Unable to open domainlog");
            goto cleanup;
        }
        closeLog = true;
    }

    if (qemuDomainLogContextWrite(logCtxt,
                                  "%s: Domain id=%d is tainted: %s\n",
                                  timestamp,
                                  obj->def->id,
                                  virDomainTaintTypeToString(taint)) < 0)
        virResetLastError();

 cleanup:
    VIR_FREE(timestamp);
    if (closeLog)
        virObjectUnref(logCtxt);
    if (orig_err) {
        virSetError(orig_err);
        virFreeError(orig_err);
    }
}


void qemuDomainObjCheckTaint(virQEMUDriverPtr driver,
                             virDomainObjPtr obj,
                             qemuDomainLogContextPtr logCtxt)
{
    size_t i;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv = obj->privateData;

    if (virQEMUDriverIsPrivileged(driver) &&
        (!cfg->clearEmulatorCapabilities ||
         cfg->user == 0 ||
         cfg->group == 0))
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_HIGH_PRIVILEGES, logCtxt);

    if (priv->hookRun)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_HOOK, logCtxt);

    if (obj->def->namespaceData) {
        qemuDomainCmdlineDefPtr qemucmd = obj->def->namespaceData;
        if (qemucmd->num_args || qemucmd->num_env)
            qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_CUSTOM_ARGV, logCtxt);
    }

    if (obj->def->cpu && obj->def->cpu->mode == VIR_CPU_MODE_HOST_PASSTHROUGH)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_HOST_CPU, logCtxt);

    for (i = 0; i < obj->def->ndisks; i++)
        qemuDomainObjCheckDiskTaint(driver, obj, obj->def->disks[i], logCtxt);

    for (i = 0; i < obj->def->nhostdevs; i++)
        qemuDomainObjCheckHostdevTaint(driver, obj, obj->def->hostdevs[i],
                                       logCtxt);

    for (i = 0; i < obj->def->nnets; i++)
        qemuDomainObjCheckNetTaint(driver, obj, obj->def->nets[i], logCtxt);

    if (obj->def->os.dtb)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_CUSTOM_DTB, logCtxt);

    virObjectUnref(cfg);
}


void qemuDomainObjCheckDiskTaint(virQEMUDriverPtr driver,
                                 virDomainObjPtr obj,
                                 virDomainDiskDefPtr disk,
                                 qemuDomainLogContextPtr logCtxt)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int format = virDomainDiskGetFormat(disk);

    if ((!format || format == VIR_STORAGE_FILE_AUTO) &&
        cfg->allowDiskFormatProbing)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_DISK_PROBING, logCtxt);

    if (disk->rawio == VIR_TRISTATE_BOOL_YES)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_HIGH_PRIVILEGES,
                           logCtxt);

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
        virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_BLOCK &&
        disk->src->path && qemuDomainFilePathIsHostCDROM(disk->src->path))
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_CDROM_PASSTHROUGH,
                           logCtxt);

    virObjectUnref(cfg);
}


void qemuDomainObjCheckHostdevTaint(virQEMUDriverPtr driver,
                                    virDomainObjPtr obj,
                                    virDomainHostdevDefPtr hostdev,
                                    qemuDomainLogContextPtr logCtxt)
{
    if (!virHostdevIsSCSIDevice(hostdev))
        return;

    if (hostdev->source.subsys.u.scsi.rawio == VIR_TRISTATE_BOOL_YES)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_HIGH_PRIVILEGES, logCtxt);
}


void qemuDomainObjCheckNetTaint(virQEMUDriverPtr driver,
                                virDomainObjPtr obj,
                                virDomainNetDefPtr net,
                                qemuDomainLogContextPtr logCtxt)
{
    /* script is only useful for NET_TYPE_ETHERNET (qemu) and
     * NET_TYPE_BRIDGE (xen), but could be (incorrectly) specified for
     * any interface type. In any case, it's adding user sauce into
     * the soup, so it should taint the domain.
     */
    if (net->script != NULL)
        qemuDomainObjTaint(driver, obj, VIR_DOMAIN_TAINT_SHELL_SCRIPTS, logCtxt);
}


qemuDomainLogContextPtr qemuDomainLogContextNew(virQEMUDriverPtr driver,
                                                virDomainObjPtr vm,
                                                qemuDomainLogContextMode mode)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    qemuDomainLogContextPtr ctxt = NULL;

    if (qemuDomainInitialize() < 0)
        goto cleanup;

    if (!(ctxt = virObjectNew(qemuDomainLogContextClass)))
        goto cleanup;

    VIR_DEBUG("Context new %p stdioLogD=%d", ctxt, cfg->stdioLogD);
    ctxt->writefd = -1;
    ctxt->readfd = -1;

    if (virAsprintf(&ctxt->path, "%s/%s.log", cfg->logDir, vm->def->name) < 0)
        goto error;

    if (cfg->stdioLogD) {
        ctxt->manager = virLogManagerNew(virQEMUDriverIsPrivileged(driver));
        if (!ctxt->manager)
            goto error;

        ctxt->writefd = virLogManagerDomainOpenLogFile(ctxt->manager,
                                                       "qemu",
                                                       vm->def->uuid,
                                                       vm->def->name,
                                                       ctxt->path,
                                                       0,
                                                       &ctxt->inode,
                                                       &ctxt->pos);
        if (ctxt->writefd < 0)
            goto error;
    } else {
        if ((ctxt->writefd = open(ctxt->path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
            virReportSystemError(errno, _("failed to create logfile %s"),
                                 ctxt->path);
            goto error;
        }
        if (virSetCloseExec(ctxt->writefd) < 0) {
            virReportSystemError(errno, _("failed to set close-on-exec flag on %s"),
                                 ctxt->path);
            goto error;
        }

        /* For unprivileged startup we must truncate the file since
         * we can't rely on logrotate. We don't use O_TRUNC since
         * it is better for SELinux policy if we truncate afterwards */
        if (mode == QEMU_DOMAIN_LOG_CONTEXT_MODE_START &&
            !virQEMUDriverIsPrivileged(driver) &&
            ftruncate(ctxt->writefd, 0) < 0) {
            virReportSystemError(errno, _("failed to truncate %s"),
                                 ctxt->path);
            goto error;
        }

        if (mode == QEMU_DOMAIN_LOG_CONTEXT_MODE_START) {
            if ((ctxt->readfd = open(ctxt->path, O_RDONLY, S_IRUSR | S_IWUSR)) < 0) {
                virReportSystemError(errno, _("failed to open logfile %s"),
                                     ctxt->path);
                goto error;
            }
            if (virSetCloseExec(ctxt->readfd) < 0) {
                virReportSystemError(errno, _("failed to set close-on-exec flag on %s"),
                                     ctxt->path);
                goto error;
            }
        }

        if ((ctxt->pos = lseek(ctxt->writefd, 0, SEEK_END)) < 0) {
            virReportSystemError(errno, _("failed to seek in log file %s"),
                                 ctxt->path);
            goto error;
        }
    }

 cleanup:
    virObjectUnref(cfg);
    return ctxt;

 error:
    virObjectUnref(ctxt);
    ctxt = NULL;
    goto cleanup;
}


int qemuDomainLogContextWrite(qemuDomainLogContextPtr ctxt,
                              const char *fmt, ...)
{
    va_list argptr;
    char *message = NULL;
    int ret = -1;

    va_start(argptr, fmt);

    if (virVasprintf(&message, fmt, argptr) < 0)
        goto cleanup;
    if (!ctxt->manager &&
        lseek(ctxt->writefd, 0, SEEK_END) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to seek to end of domain logfile"));
        goto cleanup;
    }
    if (safewrite(ctxt->writefd, message, strlen(message)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to write to domain logfile"));
        goto cleanup;
    }

    ret = 0;

 cleanup:
    va_end(argptr);
    VIR_FREE(message);
    return ret;
}


ssize_t qemuDomainLogContextRead(qemuDomainLogContextPtr ctxt,
                                 char **msg)
{
    VIR_DEBUG("Context read %p manager=%p inode=%llu pos=%llu",
              ctxt, ctxt->manager,
              (unsigned long long)ctxt->inode,
              (unsigned long long)ctxt->pos);
    char *buf;
    size_t buflen;
    if (ctxt->manager) {
        buf = virLogManagerDomainReadLogFile(ctxt->manager,
                                             ctxt->path,
                                             ctxt->inode,
                                             ctxt->pos,
                                             1024 * 128,
                                             0);
        if (!buf)
            return -1;
        buflen = strlen(buf);
    } else {
        ssize_t got;

        buflen = 1024 * 128;

        /* Best effort jump to start of messages */
        ignore_value(lseek(ctxt->readfd, ctxt->pos, SEEK_SET));

        if (VIR_ALLOC_N(buf, buflen) < 0)
            return -1;

        got = saferead(ctxt->readfd, buf, buflen - 1);
        if (got < 0) {
            VIR_FREE(buf);
            virReportSystemError(errno, "%s",
                                 _("Unable to read from log file"));
            return -1;
        }

        buf[got] = '\0';

        ignore_value(VIR_REALLOC_N_QUIET(buf, got + 1));
        buflen = got;
    }

    *msg = buf;

    return buflen;
}


/**
 * qemuDomainLogAppendMessage:
 *
 * This is a best-effort attempt to add a log message to the qemu log file
 * either by using virtlogd or the legacy approach */
int
qemuDomainLogAppendMessage(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           const char *fmt,
                           ...)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virLogManagerPtr manager = NULL;
    va_list ap;
    char *path = NULL;
    int writefd = -1;
    char *message = NULL;
    int ret = -1;

    va_start(ap, fmt);

    if (virVasprintf(&message, fmt, ap) < 0)
        goto cleanup;

    VIR_DEBUG("Append log message (vm='%s' message='%s) stdioLogD=%d",
              vm->def->name, message, cfg->stdioLogD);

    if (virAsprintf(&path, "%s/%s.log", cfg->logDir, vm->def->name) < 0)
        goto cleanup;

    if (cfg->stdioLogD) {
        if (!(manager = virLogManagerNew(virQEMUDriverIsPrivileged(driver))))
            goto cleanup;

        if (virLogManagerDomainAppendMessage(manager, "qemu", vm->def->uuid,
                                             vm->def->name, path, message, 0) < 0)
            goto cleanup;
    } else {
        if ((writefd = open(path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
            virReportSystemError(errno, _("failed to create logfile %s"),
                                 path);
            goto cleanup;
        }

        if (safewrite(writefd, message, strlen(message)) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    va_end(ap);
    VIR_FREE(message);
    VIR_FORCE_CLOSE(writefd);
    virLogManagerFree(manager);
    virObjectUnref(cfg);
    VIR_FREE(path);

    return ret;
}


int qemuDomainLogContextGetWriteFD(qemuDomainLogContextPtr ctxt)
{
    return ctxt->writefd;
}


void qemuDomainLogContextMarkPosition(qemuDomainLogContextPtr ctxt)
{
    if (ctxt->manager)
        virLogManagerDomainGetLogFilePosition(ctxt->manager,
                                              ctxt->path,
                                              0,
                                              &ctxt->inode,
                                              &ctxt->pos);
    else
        ctxt->pos = lseek(ctxt->writefd, 0, SEEK_END);
}


virLogManagerPtr qemuDomainLogContextGetManager(qemuDomainLogContextPtr ctxt)
{
    return ctxt->manager;
}


/* Locate an appropriate 'qemu-img' binary.  */
const char *
qemuFindQemuImgBinary(virQEMUDriverPtr driver)
{
    if (!driver->qemuImgBinary)
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("unable to find qemu-img"));

    return driver->qemuImgBinary;
}

int
qemuDomainSnapshotWriteMetadata(virDomainObjPtr vm,
                                virDomainSnapshotObjPtr snapshot,
                                virCapsPtr caps,
                                virDomainXMLOptionPtr xmlopt,
                                char *snapshotDir)
{
    char *newxml = NULL;
    int ret = -1;
    char *snapDir = NULL;
    char *snapFile = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(vm->def->uuid, uuidstr);
    newxml = virDomainSnapshotDefFormat(
        uuidstr, snapshot->def, caps, xmlopt,
        virDomainDefFormatConvertXMLFlags(QEMU_DOMAIN_FORMAT_LIVE_FLAGS),
        1);
    if (newxml == NULL)
        return -1;

    if (virAsprintf(&snapDir, "%s/%s", snapshotDir, vm->def->name) < 0)
        goto cleanup;
    if (virFileMakePath(snapDir) < 0) {
        virReportSystemError(errno, _("cannot create snapshot directory '%s'"),
                             snapDir);
        goto cleanup;
    }

    if (virAsprintf(&snapFile, "%s/%s.xml", snapDir, snapshot->def->name) < 0)
        goto cleanup;

    ret = virXMLSaveFile(snapFile, NULL, "snapshot-edit", newxml);

 cleanup:
    VIR_FREE(snapFile);
    VIR_FREE(snapDir);
    VIR_FREE(newxml);
    return ret;
}

/* The domain is expected to be locked and inactive. Return -1 on normal
 * failure, 1 if we skipped a disk due to try_all.  */
static int
qemuDomainSnapshotForEachQcow2Raw(virQEMUDriverPtr driver,
                                  virDomainDefPtr def,
                                  const char *name,
                                  const char *op,
                                  bool try_all,
                                  int ndisks)
{
    const char *qemuimgarg[] = { NULL, "snapshot", NULL, NULL, NULL, NULL };
    size_t i;
    bool skipped = false;

    qemuimgarg[0] = qemuFindQemuImgBinary(driver);
    if (qemuimgarg[0] == NULL) {
        /* qemuFindQemuImgBinary set the error */
        return -1;
    }

    qemuimgarg[2] = op;
    qemuimgarg[3] = name;

    for (i = 0; i < ndisks; i++) {
        /* FIXME: we also need to handle LVM here */
        if (def->disks[i]->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
            int format = virDomainDiskGetFormat(def->disks[i]);

            if (format > 0 && format != VIR_STORAGE_FILE_QCOW2) {
                if (try_all) {
                    /* Continue on even in the face of error, since other
                     * disks in this VM may have the same snapshot name.
                     */
                    VIR_WARN("skipping snapshot action on %s",
                             def->disks[i]->dst);
                    skipped = true;
                    continue;
                } else if (STREQ(op, "-c") && i) {
                    /* We must roll back partial creation by deleting
                     * all earlier snapshots.  */
                    qemuDomainSnapshotForEachQcow2Raw(driver, def, name,
                                                      "-d", false, i);
                }
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("Disk device '%s' does not support"
                                 " snapshotting"),
                               def->disks[i]->dst);
                return -1;
            }

            qemuimgarg[4] = virDomainDiskGetSource(def->disks[i]);

            if (virRun(qemuimgarg, NULL) < 0) {
                if (try_all) {
                    VIR_WARN("skipping snapshot action on %s",
                             def->disks[i]->dst);
                    skipped = true;
                    continue;
                } else if (STREQ(op, "-c") && i) {
                    /* We must roll back partial creation by deleting
                     * all earlier snapshots.  */
                    qemuDomainSnapshotForEachQcow2Raw(driver, def, name,
                                                      "-d", false, i);
                }
                return -1;
            }
        }
    }

    return skipped ? 1 : 0;
}

/* The domain is expected to be locked and inactive. Return -1 on normal
 * failure, 1 if we skipped a disk due to try_all.  */
int
qemuDomainSnapshotForEachQcow2(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               virDomainSnapshotObjPtr snap,
                               const char *op,
                               bool try_all)
{
    /* Prefer action on the disks in use at the time the snapshot was
     * created; but fall back to current definition if dealing with a
     * snapshot created prior to libvirt 0.9.5.  */
    virDomainDefPtr def = snap->def->dom;

    if (!def)
        def = vm->def;
    return qemuDomainSnapshotForEachQcow2Raw(driver, def, snap->def->name,
                                             op, try_all, def->ndisks);
}

/* Discard one snapshot (or its metadata), without reparenting any children.  */
int
qemuDomainSnapshotDiscard(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virDomainSnapshotObjPtr snap,
                          bool update_current,
                          bool metadata_only)
{
    char *snapFile = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    virDomainSnapshotObjPtr parentsnap = NULL;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (!metadata_only) {
        if (!virDomainObjIsActive(vm)) {
            /* Ignore any skipped disks */
            if (qemuDomainSnapshotForEachQcow2(driver, vm, snap, "-d",
                                               true) < 0)
                goto cleanup;
        } else {
            priv = vm->privateData;
            qemuDomainObjEnterMonitor(driver, vm);
            /* we continue on even in the face of error */
            qemuMonitorDeleteSnapshot(priv->mon, snap->def->name);
            ignore_value(qemuDomainObjExitMonitor(driver, vm));
        }
    }

    if (virAsprintf(&snapFile, "%s/%s/%s.xml", cfg->snapshotDir,
                    vm->def->name, snap->def->name) < 0)
        goto cleanup;

    if (snap == vm->current_snapshot) {
        if (update_current && snap->def->parent) {
            parentsnap = virDomainSnapshotFindByName(vm->snapshots,
                                                     snap->def->parent);
            if (!parentsnap) {
                VIR_WARN("missing parent snapshot matching name '%s'",
                         snap->def->parent);
            } else {
                parentsnap->def->current = true;
                if (qemuDomainSnapshotWriteMetadata(vm, parentsnap, driver->caps,
                                                    driver->xmlopt,
                                                    cfg->snapshotDir) < 0) {
                    VIR_WARN("failed to set parent snapshot '%s' as current",
                             snap->def->parent);
                    parentsnap->def->current = false;
                    parentsnap = NULL;
                }
            }
        }
        vm->current_snapshot = parentsnap;
    }

    if (unlink(snapFile) < 0)
        VIR_WARN("Failed to unlink %s", snapFile);
    virDomainSnapshotObjListRemove(vm->snapshots, snap);

    ret = 0;

 cleanup:
    VIR_FREE(snapFile);
    virObjectUnref(cfg);
    return ret;
}

/* Hash iterator callback to discard multiple snapshots.  */
int qemuDomainSnapshotDiscardAll(void *payload,
                                 const void *name ATTRIBUTE_UNUSED,
                                 void *data)
{
    virDomainSnapshotObjPtr snap = payload;
    virQEMUSnapRemovePtr curr = data;
    int err;

    if (snap->def->current)
        curr->current = true;
    err = qemuDomainSnapshotDiscard(curr->driver, curr->vm, snap, false,
                                    curr->metadata_only);
    if (err && !curr->err)
        curr->err = err;
    return 0;
}

int
qemuDomainSnapshotDiscardAllMetadata(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm)
{
    virQEMUSnapRemove rem;

    rem.driver = driver;
    rem.vm = vm;
    rem.metadata_only = true;
    rem.err = 0;
    virDomainSnapshotForEach(vm->snapshots, qemuDomainSnapshotDiscardAll,
                             &rem);

    return rem.err;
}


/**
 * qemuDomainRemoveInactive:
 *
 * The caller must hold a lock to the vm.
 */
void
qemuDomainRemoveInactive(virQEMUDriverPtr driver,
                         virDomainObjPtr vm)
{
    char *snapDir;
    virQEMUDriverConfigPtr cfg;

    if (vm->persistent) {
        /* Short-circuit, we don't want to remove a persistent domain */
        return;
    }

    cfg = virQEMUDriverGetConfig(driver);

    /* Remove any snapshot metadata prior to removing the domain */
    if (qemuDomainSnapshotDiscardAllMetadata(driver, vm) < 0) {
        VIR_WARN("unable to remove all snapshots for domain %s",
                 vm->def->name);
    }
    else if (virAsprintf(&snapDir, "%s/%s", cfg->snapshotDir,
                         vm->def->name) < 0) {
        VIR_WARN("unable to remove snapshot directory %s/%s",
                 cfg->snapshotDir, vm->def->name);
    } else {
        if (rmdir(snapDir) < 0 && errno != ENOENT)
            VIR_WARN("unable to remove snapshot directory %s", snapDir);
        VIR_FREE(snapDir);
    }

    virDomainObjListRemove(driver->domains, vm);

    virObjectUnref(cfg);
}


/**
 * qemuDomainRemoveInactiveJob:
 *
 * Just like qemuDomainRemoveInactive but it tries to grab a
 * QEMU_JOB_MODIFY first. Even though it doesn't succeed in
 * grabbing the job the control carries with
 * qemuDomainRemoveInactive call.
 */
void
qemuDomainRemoveInactiveJob(virQEMUDriverPtr driver,
                            virDomainObjPtr vm)
{
    bool haveJob;

    haveJob = qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) >= 0;

    qemuDomainRemoveInactive(driver, vm);

    if (haveJob)
        qemuDomainObjEndJob(driver, vm);
}


void
qemuDomainSetFakeReboot(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        bool value)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    if (priv->fakeReboot == value)
        goto cleanup;

    priv->fakeReboot = value;

    if (virDomainSaveStatus(driver->xmlopt, cfg->stateDir, vm, driver->caps) < 0)
        VIR_WARN("Failed to save status on vm %s", vm->def->name);

 cleanup:
    virObjectUnref(cfg);
}

static void
qemuDomainCheckRemoveOptionalDisk(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm,
                                  size_t diskIndex)
{
    char uuid[VIR_UUID_STRING_BUFLEN];
    virObjectEventPtr event = NULL;
    virDomainDiskDefPtr disk = vm->def->disks[diskIndex];
    const char *src = virDomainDiskGetSource(disk);

    virUUIDFormat(vm->def->uuid, uuid);

    VIR_DEBUG("Dropping disk '%s' on domain '%s' (UUID '%s') "
              "due to inaccessible source '%s'",
              disk->dst, vm->def->name, uuid, src);

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM ||
        disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY) {

        event = virDomainEventDiskChangeNewFromObj(vm, src, NULL,
                                                   disk->info.alias,
                                                   VIR_DOMAIN_EVENT_DISK_CHANGE_MISSING_ON_START);
        virDomainDiskEmptySource(disk);
        /* keeping the old startup policy would be invalid for new images */
        disk->startupPolicy = VIR_DOMAIN_STARTUP_POLICY_DEFAULT;
    } else {
        event = virDomainEventDiskChangeNewFromObj(vm, src, NULL,
                                                   disk->info.alias,
                                                   VIR_DOMAIN_EVENT_DISK_DROP_MISSING_ON_START);
        virDomainDiskRemove(vm->def, diskIndex);
        virDomainDiskDefFree(disk);
    }

    qemuDomainEventQueue(driver, event);
}


/**
 * qemuDomainCheckDiskStartupPolicy:
 * @driver: qemu driver object
 * @vm: domain object
 * @disk: index of disk to check
 * @cold_boot: true if a new VM is being started
 *
 * This function should be called when the source storage for a disk device is
 * missing. The function checks whether the startup policy for the disk allows
 * removal of the source (or disk) according to the state of the VM.
 *
 * The function returns 0 if the source or disk was dropped and -1 if the state
 * of the VM does not allow this. This function does not report errors, but
 * clears any reported error if 0 is returned.
 */
int
qemuDomainCheckDiskStartupPolicy(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 size_t diskIndex,
                                 bool cold_boot)
{
    int startupPolicy = vm->def->disks[diskIndex]->startupPolicy;
    int device = vm->def->disks[diskIndex]->device;

    switch ((virDomainStartupPolicy) startupPolicy) {
        case VIR_DOMAIN_STARTUP_POLICY_OPTIONAL:
            /* Once started with an optional disk, qemu saves its section
             * in the migration stream, so later, when restoring from it
             * we must make sure the sections match. */
            if (!cold_boot &&
                device != VIR_DOMAIN_DISK_DEVICE_FLOPPY &&
                device != VIR_DOMAIN_DISK_DEVICE_CDROM)
                return -1;
            break;

        case VIR_DOMAIN_STARTUP_POLICY_DEFAULT:
        case VIR_DOMAIN_STARTUP_POLICY_MANDATORY:
            return -1;

        case VIR_DOMAIN_STARTUP_POLICY_REQUISITE:
            if (cold_boot)
                return -1;
            break;

        case VIR_DOMAIN_STARTUP_POLICY_LAST:
            /* this should never happen */
            break;
    }

    qemuDomainCheckRemoveOptionalDisk(driver, vm, diskIndex);
    virResetLastError();
    return 0;
}



/*
 * The vm must be locked when any of the following cleanup functions is
 * called.
 */
int
qemuDomainCleanupAdd(virDomainObjPtr vm,
                     qemuDomainCleanupCallback cb)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t i;

    VIR_DEBUG("vm=%s, cb=%p", vm->def->name, cb);

    for (i = 0; i < priv->ncleanupCallbacks; i++) {
        if (priv->cleanupCallbacks[i] == cb)
            return 0;
    }

    if (VIR_RESIZE_N(priv->cleanupCallbacks,
                     priv->ncleanupCallbacks_max,
                     priv->ncleanupCallbacks, 1) < 0)
        return -1;

    priv->cleanupCallbacks[priv->ncleanupCallbacks++] = cb;
    return 0;
}

void
qemuDomainCleanupRemove(virDomainObjPtr vm,
                        qemuDomainCleanupCallback cb)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t i;

    VIR_DEBUG("vm=%s, cb=%p", vm->def->name, cb);

    for (i = 0; i < priv->ncleanupCallbacks; i++) {
        if (priv->cleanupCallbacks[i] == cb)
            VIR_DELETE_ELEMENT_INPLACE(priv->cleanupCallbacks,
                                       i, priv->ncleanupCallbacks);
    }

    VIR_SHRINK_N(priv->cleanupCallbacks,
                 priv->ncleanupCallbacks_max,
                 priv->ncleanupCallbacks_max - priv->ncleanupCallbacks);
}

void
qemuDomainCleanupRun(virQEMUDriverPtr driver,
                     virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t i;

    VIR_DEBUG("driver=%p, vm=%s", driver, vm->def->name);

    /* run cleanup callbacks in reverse order */
    for (i = 0; i < priv->ncleanupCallbacks; i++) {
        if (priv->cleanupCallbacks[priv->ncleanupCallbacks - (i + 1)])
            priv->cleanupCallbacks[i](driver, vm);
    }

    VIR_FREE(priv->cleanupCallbacks);
    priv->ncleanupCallbacks = 0;
    priv->ncleanupCallbacks_max = 0;
}

static void
qemuDomainGetImageIds(virQEMUDriverConfigPtr cfg,
                      virDomainObjPtr vm,
                      virStorageSourcePtr src,
                      virStorageSourcePtr parentSrc,
                      uid_t *uid, gid_t *gid)
{
    virSecurityLabelDefPtr vmlabel;
    virSecurityDeviceLabelDefPtr disklabel;

    if (uid)
        *uid = -1;
    if (gid)
        *gid = -1;

    if (cfg) {
        if (uid)
            *uid = cfg->user;

        if (gid)
            *gid = cfg->group;
    }

    if (vm && (vmlabel = virDomainDefGetSecurityLabelDef(vm->def, "dac")) &&
        vmlabel->label)
        virParseOwnershipIds(vmlabel->label, uid, gid);

    if (parentSrc &&
        (disklabel = virStorageSourceGetSecurityLabelDef(parentSrc, "dac")) &&
        disklabel->label)
        virParseOwnershipIds(disklabel->label, uid, gid);

    if ((disklabel = virStorageSourceGetSecurityLabelDef(src, "dac")) &&
        disklabel->label)
        virParseOwnershipIds(disklabel->label, uid, gid);
}


int
qemuDomainStorageFileInit(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          virStorageSourcePtr src,
                          virStorageSourcePtr parent)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    uid_t uid;
    gid_t gid;
    int ret = -1;

    qemuDomainGetImageIds(cfg, vm, src, parent, &uid, &gid);

    if (virStorageFileInitAs(src, uid, gid) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


char *
qemuDomainStorageAlias(const char *device, int depth)
{
    char *alias;

    device = qemuAliasDiskDriveSkipPrefix(device);

    if (!depth)
        ignore_value(VIR_STRDUP(alias, device));
    else
        ignore_value(virAsprintf(&alias, "%s.%d", device, depth));
    return alias;
}


int
qemuDomainDetermineDiskChain(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             virDomainDiskDefPtr disk,
                             bool force_probe,
                             bool report_broken)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    virStorageSourcePtr src = disk->src;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    uid_t uid;
    gid_t gid;

    if (virStorageSourceIsEmpty(src)) {
        ret = 0;
        goto cleanup;
    }

    if (force_probe)
        virStorageSourceBackingStoreClear(src);

    /* There is no need to check the backing chain for disks without backing
     * support */
    if (virStorageSourceIsLocalStorage(src) &&
        src->format > VIR_STORAGE_FILE_NONE &&
        src->format < VIR_STORAGE_FILE_BACKING) {

        if (!virFileExists(src->path)) {
            if (report_broken)
                virStorageFileReportBrokenChain(errno, src, disk->src);

            goto cleanup;
        }

        /* terminate the chain for such images as the code below would do */
        if (!src->backingStore &&
            VIR_ALLOC(src->backingStore) < 0)
            goto cleanup;

        /* host cdrom requires special treatment in qemu, so we need to check
         * whether a block device is a cdrom */
        if (disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM &&
            src->format == VIR_STORAGE_FILE_RAW &&
            virStorageSourceIsBlockLocal(src) &&
            virFileIsCDROM(src->path) == 1)
            src->hostcdrom = true;

        ret = 0;
        goto cleanup;
    }

    /* skip to the end of the chain if there is any */
    while (virStorageSourceHasBacking(src)) {
        if (report_broken) {
            int rv = virStorageFileSupportsAccess(src);

            if (rv < 0)
                goto cleanup;

            if (rv > 0) {
                if (qemuDomainStorageFileInit(driver, vm, src, disk->src) < 0)
                    goto cleanup;

                if (virStorageFileAccess(src, F_OK) < 0) {
                    virStorageFileReportBrokenChain(errno, src, disk->src);
                    virStorageFileDeinit(src);
                    goto cleanup;
                }

                virStorageFileDeinit(src);
            }
        }
        src = src->backingStore;
    }

    /* We skipped to the end of the chain. Skip detection if there's the
     * terminator. (An allocated but empty backingStore) */
    if (src->backingStore) {
        ret = 0;
        goto cleanup;
    }

    qemuDomainGetImageIds(cfg, vm, src, disk->src, &uid, &gid);

    if (virStorageFileGetMetadata(src,
                                  uid, gid,
                                  cfg->allowDiskFormatProbing,
                                  report_broken) < 0)
        goto cleanup;

    /* fill in data for the rest of the chain */
    if (qemuDomainPrepareDiskSourceChain(disk, src, cfg, priv->qemuCaps) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virObjectUnref(cfg);
    return ret;
}


/**
 * qemuDomainDiskChainElementRevoke:
 *
 * Revoke access to a single backing chain element. This restores the labels,
 * removes cgroup ACLs for devices and removes locks.
 */
void
qemuDomainDiskChainElementRevoke(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virStorageSourcePtr elem)
{
    if (qemuTeardownImageCgroup(vm, elem) < 0)
        VIR_WARN("Failed to teardown cgroup for disk path %s",
                 NULLSTR(elem->path));

    if (qemuSecurityRestoreImageLabel(driver, vm, elem) < 0)
        VIR_WARN("Unable to restore security label on %s", NULLSTR(elem->path));

    if (qemuDomainNamespaceTeardownDisk(vm, elem) < 0)
        VIR_WARN("Unable to remove /dev entry for %s", NULLSTR(elem->path));

    if (virDomainLockImageDetach(driver->lockManager, vm, elem) < 0)
        VIR_WARN("Unable to release lock on %s", NULLSTR(elem->path));
}


/**
 * qemuDomainDiskChainElementPrepare:
 * @driver: qemu driver data
 * @vm: domain object
 * @elem: source structure to set access for
 * @readonly: setup read-only access if true
 * @newSource: @elem describes a storage source which @vm can't access yet
 *
 * Allow a VM access to a single element of a disk backing chain; this helper
 * ensures that the lock manager, cgroup device controller, and security manager
 * labelling are all aware of each new file before it is added to a chain.
 *
 * When modifying permissions of @elem which @vm can already access (is in the
 * backing chain) @newSource needs to be set to false.
 */
int
qemuDomainDiskChainElementPrepare(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm,
                                  virStorageSourcePtr elem,
                                  bool readonly,
                                  bool newSource)
{
    bool was_readonly = elem->readonly;
    virQEMUDriverConfigPtr cfg = NULL;
    int ret = -1;

    cfg = virQEMUDriverGetConfig(driver);

    elem->readonly = readonly;

    if (virDomainLockImageAttach(driver->lockManager, cfg->uri, vm, elem) < 0)
        goto cleanup;

    if (newSource &&
        qemuDomainNamespaceSetupDisk(vm, elem) < 0)
        goto cleanup;

    if (qemuSetupImageCgroup(vm, elem) < 0)
        goto cleanup;

    if (qemuSecuritySetImageLabel(driver, vm, elem) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    elem->readonly = was_readonly;
    virObjectUnref(cfg);
    return ret;
}


bool
qemuDomainDiskSourceDiffers(virDomainDiskDefPtr disk,
                            virDomainDiskDefPtr origDisk)
{
    char *diskSrc = NULL, *origDiskSrc = NULL;
    bool diskEmpty, origDiskEmpty;
    bool ret = true;

    diskEmpty = virStorageSourceIsEmpty(disk->src);
    origDiskEmpty = virStorageSourceIsEmpty(origDisk->src);

    if (diskEmpty && origDiskEmpty)
        return false;

    if (diskEmpty ^ origDiskEmpty)
        return true;

    /* This won't be a network storage, so no need to get the diskPriv
     * in order to fetch the secret, thus NULL for param2 */
    if (qemuGetDriveSourceString(disk->src, NULL, &diskSrc) < 0 ||
        qemuGetDriveSourceString(origDisk->src, NULL, &origDiskSrc) < 0)
        goto cleanup;

    /* So far in qemu disk sources are considered different
     * if either path to disk or its format changes. */
    ret = virDomainDiskGetFormat(disk) != virDomainDiskGetFormat(origDisk) ||
          STRNEQ_NULLABLE(diskSrc, origDiskSrc);
 cleanup:
    VIR_FREE(diskSrc);
    VIR_FREE(origDiskSrc);
    return ret;
}


/*
 * Makes sure the @disk differs from @orig_disk only by the source
 * path and nothing else.  Fields that are being checked and the
 * information whether they are nullable (may not be specified) or is
 * taken from the virDomainDiskDefFormat() code.
 */
bool
qemuDomainDiskChangeSupported(virDomainDiskDefPtr disk,
                              virDomainDiskDefPtr orig_disk)
{
#define CHECK_EQ(field, field_name, nullable) \
    do { \
        if (nullable && !disk->field) \
            break; \
        if (disk->field != orig_disk->field) { \
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, \
                           _("cannot modify field '%s' of the disk"), \
                           field_name); \
            return false; \
        } \
    } while (0)

    CHECK_EQ(device, "device", false);
    CHECK_EQ(bus, "bus", false);
    if (STRNEQ(disk->dst, orig_disk->dst)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "target");
        return false;
    }
    CHECK_EQ(tray_status, "tray", true);
    CHECK_EQ(removable, "removable", true);

    if (disk->geometry.cylinders &&
        disk->geometry.heads &&
        disk->geometry.sectors) {
        CHECK_EQ(geometry.cylinders, "geometry cylinders", false);
        CHECK_EQ(geometry.heads, "geometry heads", false);
        CHECK_EQ(geometry.sectors, "geometry sectors", false);
        CHECK_EQ(geometry.trans, "BIOS-translation-modus", true);
    }

    CHECK_EQ(blockio.logical_block_size,
             "blockio logical_block_size", false);
    CHECK_EQ(blockio.physical_block_size,
             "blockio physical_block_size", false);

    CHECK_EQ(blkdeviotune.total_bytes_sec,
             "blkdeviotune total_bytes_sec",
             true);
    CHECK_EQ(blkdeviotune.read_bytes_sec,
             "blkdeviotune read_bytes_sec",
             true);
    CHECK_EQ(blkdeviotune.write_bytes_sec,
             "blkdeviotune write_bytes_sec",
             true);
    CHECK_EQ(blkdeviotune.total_iops_sec,
             "blkdeviotune total_iops_sec",
             true);
    CHECK_EQ(blkdeviotune.read_iops_sec,
             "blkdeviotune read_iops_sec",
             true);
    CHECK_EQ(blkdeviotune.write_iops_sec,
             "blkdeviotune write_iops_sec",
             true);
    CHECK_EQ(blkdeviotune.total_bytes_sec_max,
             "blkdeviotune total_bytes_sec_max",
             true);
    CHECK_EQ(blkdeviotune.read_bytes_sec_max,
             "blkdeviotune read_bytes_sec_max",
             true);
    CHECK_EQ(blkdeviotune.write_bytes_sec_max,
             "blkdeviotune write_bytes_sec_max",
             true);
    CHECK_EQ(blkdeviotune.total_iops_sec_max,
             "blkdeviotune total_iops_sec_max",
             true);
    CHECK_EQ(blkdeviotune.read_iops_sec_max,
             "blkdeviotune read_iops_sec_max",
             true);
    CHECK_EQ(blkdeviotune.write_iops_sec_max,
             "blkdeviotune write_iops_sec_max",
             true);
    CHECK_EQ(blkdeviotune.size_iops_sec,
             "blkdeviotune size_iops_sec",
             true);

    if (disk->serial && STRNEQ_NULLABLE(disk->serial, orig_disk->serial)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "serial");
        return false;
    }

    if (disk->wwn && STRNEQ_NULLABLE(disk->wwn, orig_disk->wwn)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "wwn");
        return false;
    }

    if (disk->vendor && STRNEQ_NULLABLE(disk->vendor, orig_disk->vendor)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "vendor");
        return false;
    }

    if (disk->product && STRNEQ_NULLABLE(disk->product, orig_disk->product)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "product");
        return false;
    }

    CHECK_EQ(cachemode, "cache", true);
    CHECK_EQ(error_policy, "error_policy", true);
    CHECK_EQ(rerror_policy, "rerror_policy", true);
    CHECK_EQ(iomode, "io", true);
    CHECK_EQ(ioeventfd, "ioeventfd", true);
    CHECK_EQ(event_idx, "event_idx", true);
    CHECK_EQ(copy_on_read, "copy_on_read", true);
    /* "snapshot" is a libvirt internal field and thus can be changed */
    /* startupPolicy is allowed to be updated. Therefore not checked here. */
    CHECK_EQ(transient, "transient", true);

    /* Note: For some address types the address auto generation for
     * @disk has still not happened at this point (e.g. driver
     * specific addresses) therefore we can't catch these possible
     * address modifications here. */
    if (disk->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
        !virDomainDeviceInfoAddressIsEqual(&disk->info, &orig_disk->info)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "address");
        return false;
    }

    if (disk->info.alias &&
        STRNEQ_NULLABLE(disk->info.alias, orig_disk->info.alias)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "alias");
        return false;
    }

    CHECK_EQ(info.bootIndex, "boot order", true);
    CHECK_EQ(rawio, "rawio", true);
    CHECK_EQ(sgio, "sgio", true);
    CHECK_EQ(discard, "discard", true);
    CHECK_EQ(iothread, "iothread", true);

    if (disk->domain_name &&
        STRNEQ_NULLABLE(disk->domain_name, orig_disk->domain_name)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("cannot modify field '%s' of the disk"),
                       "backenddomain");
        return false;
    }

    /* checks for fields stored in disk->src */
    /* unfortunately 'readonly' and 'shared' can't be converted to tristate
     * values thus we need to ignore the check if the new value is 'false' */
    CHECK_EQ(src->readonly, "readonly", true);
    CHECK_EQ(src->shared, "shared", true);

#undef CHECK_EQ

    return true;
}

bool
qemuDomainDiskBlockJobIsActive(virDomainDiskDefPtr disk)
{
    qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);

    if (disk->mirror) {
        virReportError(VIR_ERR_BLOCK_COPY_ACTIVE,
                       _("disk '%s' already in active block job"),
                       disk->dst);

        return true;
    }

    if (diskPriv->blockjob) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("disk '%s' already in active block job"),
                       disk->dst);
        return true;
    }

    return false;
}


/**
 * qemuDomainHasBlockjob:
 * @vm: domain object
 * @copy_only: Reject only block copy job
 *
 * Return true if @vm has at least one disk involved in a current block
 * copy/commit/pull job. If @copy_only is true this returns true only if the
 * disk is involved in a block copy.
 * */
bool
qemuDomainHasBlockjob(virDomainObjPtr vm,
                      bool copy_only)
{
    size_t i;
    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDefPtr disk = vm->def->disks[i];
        qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);

        if (!copy_only && diskPriv->blockjob)
            return true;

        if (disk->mirror && disk->mirrorJob == VIR_DOMAIN_BLOCK_JOB_TYPE_COPY)
            return true;
    }

    return false;
}


int
qemuDomainUpdateDeviceList(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           int asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    char **aliases;
    int rc;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DEVICE_DEL_EVENT))
        return 0;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;
    rc = qemuMonitorGetDeviceAliases(priv->mon, &aliases);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;
    if (rc < 0)
        return -1;

    virStringListFree(priv->qemuDevices);
    priv->qemuDevices = aliases;
    return 0;
}


int
qemuDomainUpdateMemoryDeviceInfo(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 int asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virHashTablePtr meminfo = NULL;
    int rc;
    size_t i;

    if (vm->def->nmems == 0)
        return 0;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    rc = qemuMonitorGetMemoryDeviceInfo(priv->mon, &meminfo);

    if (qemuDomainObjExitMonitor(driver, vm) < 0) {
        virHashFree(meminfo);
        return -1;
    }

    /* if qemu doesn't support the info request, just carry on */
    if (rc == -2)
        return 0;

    if (rc < 0)
        return -1;

    for (i = 0; i < vm->def->nmems; i++) {
        virDomainMemoryDefPtr mem = vm->def->mems[i];
        qemuMonitorMemoryDeviceInfoPtr dimm;

        if (!mem->info.alias)
            continue;

        if (!(dimm = virHashLookup(meminfo, mem->info.alias)))
            continue;

        mem->info.type = VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM;
        mem->info.addr.dimm.slot = dimm->slot;
        mem->info.addr.dimm.base = dimm->address;
    }

    virHashFree(meminfo);
    return 0;
}


static bool
qemuDomainABIStabilityCheck(const virDomainDef *src,
                            const virDomainDef *dst)
{
    size_t i;

    if (src->mem.source != dst->mem.source) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Target memoryBacking source '%s' doesn't "
                         "match source memoryBacking source'%s'"),
                       virDomainMemorySourceTypeToString(dst->mem.source),
                       virDomainMemorySourceTypeToString(src->mem.source));
        return false;
    }

    for (i = 0; i < src->nmems; i++) {
        const char *srcAlias = src->mems[i]->info.alias;
        const char *dstAlias = dst->mems[i]->info.alias;

        if (STRNEQ_NULLABLE(srcAlias, dstAlias)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Target memory device alias '%s' doesn't "
                             "match source alias '%s'"),
                           NULLSTR(srcAlias), NULLSTR(dstAlias));
            return false;
        }
    }

    return true;
}


virDomainABIStability virQEMUDriverDomainABIStability = {
    .domain = qemuDomainABIStabilityCheck,
};


static bool
qemuDomainMigratableDefCheckABIStability(virQEMUDriverPtr driver,
                                         virDomainDefPtr src,
                                         virDomainDefPtr migratableSrc,
                                         virDomainDefPtr dst,
                                         virDomainDefPtr migratableDst)
{
    if (!virDomainDefCheckABIStabilityFlags(migratableSrc,
                                            migratableDst,
                                            driver->xmlopt,
                                            VIR_DOMAIN_DEF_ABI_CHECK_SKIP_VOLATILE))
        return false;

    /* Force update any skipped values from the volatile flag */
    dst->mem.cur_balloon = src->mem.cur_balloon;

    return true;
}


#define COPY_FLAGS (VIR_DOMAIN_XML_SECURE | \
                    VIR_DOMAIN_XML_MIGRATABLE)

bool
qemuDomainDefCheckABIStability(virQEMUDriverPtr driver,
                               virDomainDefPtr src,
                               virDomainDefPtr dst)
{
    virDomainDefPtr migratableDefSrc = NULL;
    virDomainDefPtr migratableDefDst = NULL;
    bool ret = false;

    if (!(migratableDefSrc = qemuDomainDefCopy(driver, src, COPY_FLAGS)) ||
        !(migratableDefDst = qemuDomainDefCopy(driver, dst, COPY_FLAGS)))
        goto cleanup;

    ret = qemuDomainMigratableDefCheckABIStability(driver,
                                                   src, migratableDefSrc,
                                                   dst, migratableDefDst);

 cleanup:
    virDomainDefFree(migratableDefSrc);
    virDomainDefFree(migratableDefDst);
    return ret;
}


bool
qemuDomainCheckABIStability(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virDomainDefPtr dst)
{
    virDomainDefPtr migratableSrc = NULL;
    virDomainDefPtr migratableDst = NULL;
    char *xml = NULL;
    bool ret = false;

    if (!(xml = qemuDomainFormatXML(driver, vm, COPY_FLAGS)) ||
        !(migratableSrc = qemuDomainDefFromXML(driver, xml)) ||
        !(migratableDst = qemuDomainDefCopy(driver, dst, COPY_FLAGS)))
        goto cleanup;

    ret = qemuDomainMigratableDefCheckABIStability(driver,
                                                   vm->def, migratableSrc,
                                                   dst, migratableDst);

 cleanup:
    VIR_FREE(xml);
    virDomainDefFree(migratableSrc);
    virDomainDefFree(migratableDst);
    return ret;
}

#undef COPY_FLAGS


bool
qemuDomainAgentAvailable(virDomainObjPtr vm,
                         bool reportError)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        if (reportError) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("domain is not running"));
        }
        return false;
    }
    if (priv->agentError) {
        if (reportError) {
            virReportError(VIR_ERR_AGENT_UNRESPONSIVE, "%s",
                           _("QEMU guest agent is not "
                             "available due to an error"));
        }
        return false;
    }
    if (!priv->agent) {
        if (qemuFindAgentConfig(vm->def)) {
            if (reportError) {
                virReportError(VIR_ERR_AGENT_UNRESPONSIVE, "%s",
                               _("QEMU guest agent is not connected"));
            }
            return false;
        } else {
            if (reportError) {
                virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                               _("QEMU guest agent is not configured"));
            }
            return false;
        }
    }
    return true;
}


static unsigned long long
qemuDomainGetMemorySizeAlignment(virDomainDefPtr def)
{
    /* PPC requires the memory sizes to be rounded to 256MiB increments, so
     * round them to the size always. */
    if (ARCH_IS_PPC64(def->os.arch))
        return 256 * 1024;

    /* Align memory size. QEMU requires rounding to next 4KiB block.
     * We'll take the "traditional" path and round it to 1MiB*/

    return 1024;
}


static unsigned long long
qemuDomainGetMemoryModuleSizeAlignment(const virDomainDef *def,
                                       const virDomainMemoryDef *mem ATTRIBUTE_UNUSED)
{
    /* PPC requires the memory sizes to be rounded to 256MiB increments, so
     * round them to the size always. */
    if (ARCH_IS_PPC64(def->os.arch))
        return 256 * 1024;

    /* dimm memory modules require 2MiB alignment rather than the 1MiB we are
     * using elsewhere. */
    return 2048;
}


int
qemuDomainAlignMemorySizes(virDomainDefPtr def)
{
    unsigned long long maxmemkb = virMemoryMaxValue(false) >> 10;
    unsigned long long maxmemcapped = virMemoryMaxValue(true) >> 10;
    unsigned long long initialmem = 0;
    unsigned long long hotplugmem = 0;
    unsigned long long mem;
    unsigned long long align = qemuDomainGetMemorySizeAlignment(def);
    size_t ncells = virDomainNumaGetNodeCount(def->numa);
    size_t i;

    /* align NUMA cell sizes if relevant */
    for (i = 0; i < ncells; i++) {
        mem = VIR_ROUND_UP(virDomainNumaGetNodeMemorySize(def->numa, i), align);
        initialmem += mem;

        if (mem > maxmemkb) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("memory size of NUMA node '%zu' overflowed after "
                             "alignment"), i);
            return -1;
        }
        virDomainNumaSetNodeMemorySize(def->numa, i, mem);
    }

    /* align initial memory size, if NUMA is present calculate it as total of
     * individual aligned NUMA node sizes */
    if (initialmem == 0)
        initialmem = VIR_ROUND_UP(virDomainDefGetMemoryInitial(def), align);

    if (initialmem > maxmemcapped) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("initial memory size overflowed after alignment"));
        return -1;
    }

    def->mem.max_memory = VIR_ROUND_UP(def->mem.max_memory, align);
    if (def->mem.max_memory > maxmemkb) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("maximum memory size overflowed after alignment"));
        return -1;
    }

    /* Align memory module sizes */
    for (i = 0; i < def->nmems; i++) {
        align = qemuDomainGetMemoryModuleSizeAlignment(def, def->mems[i]);
        def->mems[i]->size = VIR_ROUND_UP(def->mems[i]->size, align);
        hotplugmem += def->mems[i]->size;

        if (def->mems[i]->size > maxmemkb) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("size of memory module '%zu' overflowed after "
                             "alignment"), i);
            return -1;
        }
    }

    virDomainDefSetMemoryTotal(def, initialmem + hotplugmem);

    return 0;
}


/**
 * qemuDomainMemoryDeviceAlignSize:
 * @mem: memory device definition object
 *
 * Aligns the size of the memory module as qemu enforces it. The size is updated
 * inplace. Default rounding is now to 1 MiB (qemu requires rouding to page,
 * size so this should be safe).
 */
void
qemuDomainMemoryDeviceAlignSize(virDomainDefPtr def,
                                virDomainMemoryDefPtr mem)
{
    mem->size = VIR_ROUND_UP(mem->size, qemuDomainGetMemorySizeAlignment(def));
}


/**
 * qemuDomainGetMonitor:
 * @vm: domain object
 *
 * Returns the monitor pointer corresponding to the domain object @vm.
 */
qemuMonitorPtr
qemuDomainGetMonitor(virDomainObjPtr vm)
{
    return ((qemuDomainObjPrivatePtr) vm->privateData)->mon;
}


/**
 * qemuDomainSupportsBlockJobs:
 * @vm: domain object
 *
 * Returns -1 in case when qemu does not support block jobs at all. Otherwise
 * returns 0.
 */
int
qemuDomainSupportsBlockJobs(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool asynchronous = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKJOB_ASYNC);

    if (!asynchronous) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("block jobs not supported with this QEMU binary"));
        return -1;
    }

    return 0;
}


/**
 * qemuFindAgentConfig:
 * @def: domain definition
 *
 * Returns the pointer to the channel definition that is used to access the
 * guest agent if the agent is configured or NULL otherwise.
 */
virDomainChrDefPtr
qemuFindAgentConfig(virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->nchannels; i++) {
        virDomainChrDefPtr channel = def->channels[i];

        if (channel->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO)
            continue;

        if (STREQ_NULLABLE(channel->target.name, "org.qemu.guest_agent.0"))
            return channel;
    }

    return NULL;
}


bool
qemuDomainIsQ35(const virDomainDef *def)
{
    return qemuDomainMachineIsQ35(def->os.machine);
}


bool
qemuDomainMachineIsQ35(const char *machine)
{
    return (STRPREFIX(machine, "pc-q35") ||
            STREQ(machine, "q35"));
}


bool
qemuDomainIsI440FX(const virDomainDef *def)
{
    return qemuDomainMachineIsI440FX(def->os.machine);
}


bool
qemuDomainMachineIsI440FX(const char *machine)
{
    return (STREQ(machine, "pc") ||
            STRPREFIX(machine, "pc-0.") ||
            STRPREFIX(machine, "pc-1.") ||
            STRPREFIX(machine, "pc-i440") ||
            STRPREFIX(machine, "rhel"));
}


bool
qemuDomainHasPCIRoot(const virDomainDef *def)
{
    int root = virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0);

    if (root < 0)
        return false;

    if (def->controllers[root]->model != VIR_DOMAIN_CONTROLLER_MODEL_PCI_ROOT)
        return false;

    return true;
}


bool
qemuDomainHasPCIeRoot(const virDomainDef *def)
{
    int root = virDomainControllerFind(def, VIR_DOMAIN_CONTROLLER_TYPE_PCI, 0);

    if (root < 0)
        return false;

    if (def->controllers[root]->model != VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT)
        return false;

    return true;
}


bool
qemuDomainNeedsFDC(const virDomainDef *def)
{
    return qemuDomainMachineNeedsFDC(def->os.machine);
}


bool
qemuDomainMachineNeedsFDC(const char *machine)
{
    const char *p = STRSKIP(machine, "pc-q35-");

    if (p) {
        if (STRPREFIX(p, "1.") ||
            STRPREFIX(p, "2.0") ||
            STRPREFIX(p, "2.1") ||
            STRPREFIX(p, "2.2") ||
            STRPREFIX(p, "2.3"))
            return false;
        return true;
    }
    return false;
}


bool
qemuDomainIsS390CCW(const virDomainDef *def)
{
    return qemuDomainMachineIsS390CCW(def->os.machine);
}


bool
qemuDomainMachineIsS390CCW(const char *machine)
{
    return STRPREFIX(machine, "s390-ccw");
}


bool
qemuDomainIsVirt(const virDomainDef *def)
{
    return qemuDomainMachineIsVirt(def->os.machine, def->os.arch);
}


bool
qemuDomainMachineIsVirt(const char *machine,
                        const virArch arch)
{
    if (arch != VIR_ARCH_ARMV7L &&
        arch != VIR_ARCH_AARCH64)
        return false;

    if (STRNEQ(machine, "virt") &&
        !STRPREFIX(machine, "virt-"))
        return false;

    return true;
}


bool
qemuDomainIsPSeries(const virDomainDef *def)
{
    return qemuDomainMachineIsPSeries(def->os.machine, def->os.arch);
}


bool
qemuDomainMachineIsPSeries(const char *machine,
                           const virArch arch)
{
    if (!ARCH_IS_PPC64(arch))
        return false;

    if (STRNEQ(machine, "pseries") &&
        !STRPREFIX(machine, "pseries-"))
        return false;

    return true;
}


static bool
qemuCheckMemoryDimmConflict(const virDomainDef *def,
                            const virDomainMemoryDef *mem)
{
    size_t i;

    for (i = 0; i < def->nmems; i++) {
         virDomainMemoryDefPtr tmp = def->mems[i];

         if (tmp == mem ||
             tmp->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM)
             continue;

         if (mem->info.addr.dimm.slot == tmp->info.addr.dimm.slot) {
             virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                            _("memory device slot '%u' is already being "
                              "used by another memory device"),
                            mem->info.addr.dimm.slot);
             return true;
         }

         if (mem->info.addr.dimm.base != 0 &&
             mem->info.addr.dimm.base == tmp->info.addr.dimm.base) {
             virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                            _("memory device base '0x%llx' is already being "
                              "used by another memory device"),
                            mem->info.addr.dimm.base);
             return true;
         }
    }

    return false;
}
static int
qemuDomainDefValidateMemoryHotplugDevice(const virDomainMemoryDef *mem,
                                         const virDomainDef *def)
{
    switch ((virDomainMemoryModel) mem->model) {
    case VIR_DOMAIN_MEMORY_MODEL_DIMM:
    case VIR_DOMAIN_MEMORY_MODEL_NVDIMM:
        if (mem->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM &&
            mem->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("only 'dimm' addresses are supported for the "
                             "pc-dimm device"));
            return -1;
        }

        if (virDomainNumaGetNodeCount(def->numa) != 0) {
            if (mem->targetNode == -1) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("target NUMA node needs to be specified for "
                                 "memory device"));
                return -1;
            }
        }

        if (mem->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM) {
            if (mem->info.addr.dimm.slot >= def->mem.memory_slots) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("memory device slot '%u' exceeds slots "
                                 "count '%u'"),
                               mem->info.addr.dimm.slot, def->mem.memory_slots);
                return -1;
            }


            if (qemuCheckMemoryDimmConflict(def, mem))
                return -1;
        }
        break;

    case VIR_DOMAIN_MEMORY_MODEL_NONE:
    case VIR_DOMAIN_MEMORY_MODEL_LAST:
        return -1;
    }

    return 0;
}


/**
 * qemuDomainDefValidateMemoryHotplug:
 * @def: domain definition
 * @qemuCaps: qemu capabilities object
 * @mem: definition of memory device that is to be added to @def with hotplug,
 *       NULL in case of regular VM startup
 *
 * Validates that the domain definition and memory modules have valid
 * configuration and are possibly able to accept @mem via hotplug if it's
 * non-NULL.
 *
 * Returns 0 on success; -1 and a libvirt error on error.
 */
int
qemuDomainDefValidateMemoryHotplug(const virDomainDef *def,
                                   virQEMUCapsPtr qemuCaps,
                                   const virDomainMemoryDef *mem)
{
    unsigned int nmems = def->nmems;
    unsigned long long hotplugSpace;
    unsigned long long hotplugMemory = 0;
    bool needPCDimmCap = false;
    bool needNvdimmCap = false;
    size_t i;

    hotplugSpace = def->mem.max_memory - virDomainDefGetMemoryInitial(def);

    if (mem) {
        nmems++;
        hotplugMemory = mem->size;

        if (qemuDomainDefValidateMemoryHotplugDevice(mem, def) < 0)
            return -1;
    }

    if (!virDomainDefHasMemoryHotplug(def)) {
        if (nmems) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("cannot use/hotplug a memory device when domain "
                             "'maxMemory' is not defined"));
            return -1;
        }

        return 0;
    }

    if (!ARCH_IS_PPC64(def->os.arch)) {
        /* due to guest support, qemu would silently enable NUMA with one node
         * once the memory hotplug backend is enabled. To avoid possible
         * confusion we will enforce user originated numa configuration along
         * with memory hotplug. */
        if (virDomainNumaGetNodeCount(def->numa) == 0) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("At least one numa node has to be configured when "
                             "enabling memory hotplug"));
            return -1;
        }
    }

    if (nmems > def->mem.memory_slots) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("memory device count '%u' exceeds slots count '%u'"),
                       nmems, def->mem.memory_slots);
        return -1;
    }

    for (i = 0; i < def->nmems; i++) {
        hotplugMemory += def->mems[i]->size;

        switch ((virDomainMemoryModel) def->mems[i]->model) {
        case VIR_DOMAIN_MEMORY_MODEL_DIMM:
            needPCDimmCap = true;
            break;

        case VIR_DOMAIN_MEMORY_MODEL_NVDIMM:
            needNvdimmCap = true;
            break;

        case VIR_DOMAIN_MEMORY_MODEL_NONE:
        case VIR_DOMAIN_MEMORY_MODEL_LAST:
            break;
        }

        /* already existing devices don't need to be checked on hotplug */
        if (!mem &&
            qemuDomainDefValidateMemoryHotplugDevice(def->mems[i], def) < 0)
            return -1;
    }

    if (needPCDimmCap &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_PC_DIMM)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("memory hotplug isn't supported by this QEMU binary"));
        return -1;
    }

    if (needNvdimmCap &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_NVDIMM)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("nvdimm isn't supported by this QEMU binary"));
        return -1;
    }

    if (hotplugMemory > hotplugSpace) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("memory device total size exceeds hotplug space"));
        return -1;
    }

    return 0;
}


bool
qemuDomainHasBuiltinIDE(const virDomainDef *def)
{
    return qemuDomainMachineHasBuiltinIDE(def->os.machine);
}


bool
qemuDomainMachineHasBuiltinIDE(const char *machine)
{
    return qemuDomainMachineIsI440FX(machine) ||
        STREQ(machine, "malta") ||
        STREQ(machine, "sun4u") ||
        STREQ(machine, "g3beige");
}


/**
 * qemuDomainUpdateCurrentMemorySize:
 *
 * Updates the current balloon size from the monitor if necessary. In case when
 * the balloon is not present for the domain, the function recalculates the
 * maximum size to reflect possible changes.
 *
 * Returns 0 on success and updates vm->def->mem.cur_balloon if necessary, -1 on
 * error and reports libvirt error.
 */
int
qemuDomainUpdateCurrentMemorySize(virQEMUDriverPtr driver,
                                  virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    unsigned long long balloon;
    int ret = -1;

    /* inactive domain doesn't need size update */
    if (!virDomainObjIsActive(vm))
        return 0;

    /* if no balloning is available, the current size equals to the current
     * full memory size */
    if (!virDomainDefHasMemballoon(vm->def)) {
        vm->def->mem.cur_balloon = virDomainDefGetMemoryTotal(vm->def);
        return 0;
    }

    /* current size is always automagically updated via the event */
    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BALLOON_EVENT))
        return 0;

    /* here we need to ask the monitor */

    /* Don't delay if someone's using the monitor, just use existing most
     * recent data instead */
    if (qemuDomainJobAllowed(priv, QEMU_JOB_QUERY)) {
        if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
            return -1;

        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("domain is not running"));
            goto endjob;
        }

        qemuDomainObjEnterMonitor(driver, vm);
        ret = qemuMonitorGetBalloonInfo(priv->mon, &balloon);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            ret = -1;

 endjob:
        qemuDomainObjEndJob(driver, vm);

        if (ret < 0)
            return -1;

        vm->def->mem.cur_balloon = balloon;
    }

    return 0;
}


/**
 * qemuDomainGetMemLockLimitBytes:
 * @def: domain definition
 *
 * Calculate the memory locking limit that needs to be set in order for
 * the guest to operate properly. The limit depends on a number of factors,
 * including certain configuration options and less immediately apparent ones
 * such as the guest architecture or the use of certain devices.
 *
 * Returns: the memory locking limit, or 0 if setting the limit is not needed
 */
unsigned long long
qemuDomainGetMemLockLimitBytes(virDomainDefPtr def)
{
    unsigned long long memKB = 0;
    size_t i;

    /* prefer the hard limit */
    if (virMemoryLimitIsSet(def->mem.hard_limit)) {
        memKB = def->mem.hard_limit;
        goto done;
    }

    /* If the guest wants its memory to be locked, we need to raise the memory
     * locking limit so that the OS will not refuse allocation requests;
     * however, there is no reliable way for us to figure out how much memory
     * the QEMU process will allocate for its own use, so our only way out is
     * to remove the limit altogether. Use with extreme care */
    if (def->mem.locked)
        return VIR_DOMAIN_MEMORY_PARAM_UNLIMITED;

    if (ARCH_IS_PPC64(def->os.arch) && def->virtType == VIR_DOMAIN_VIRT_KVM) {
        unsigned long long maxMemory;
        unsigned long long memory;
        unsigned long long baseLimit;
        unsigned long long passthroughLimit;
        size_t nPCIHostBridges = 0;
        bool usesVFIO = false;

        for (i = 0; i < def->ncontrollers; i++) {
            virDomainControllerDefPtr cont = def->controllers[i];

            if (!virDomainControllerIsPSeriesPHB(cont))
                continue;

            nPCIHostBridges++;
        }

        for (i = 0; i < def->nhostdevs; i++) {
            virDomainHostdevDefPtr dev = def->hostdevs[i];

            if (dev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
                dev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
                dev->source.subsys.u.pci.backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                usesVFIO = true;
                break;
            }
        }

        memory = virDomainDefGetMemoryTotal(def);

        if (def->mem.max_memory)
            maxMemory = def->mem.max_memory;
        else
            maxMemory = memory;

        /* baseLimit := maxMemory / 128                                  (a)
         *              + 4 MiB * #PHBs + 8 MiB                          (b)
         *
         * (a) is the hash table
         *
         * (b) is accounting for the 32-bit DMA window - it could be either the
         * KVM accelerated TCE tables for emulated devices, or the VFIO
         * userspace view. The 4 MiB per-PHB (including the default one) covers
         * a 2GiB DMA window: default is 1GiB, but it's possible it'll be
         * increased to help performance. The 8 MiB extra should be plenty for
         * the TCE table index for any reasonable number of PHBs and several
         * spapr-vlan or spapr-vscsi devices (512kB + a tiny bit each) */
        baseLimit = maxMemory / 128 +
                    4096 * nPCIHostBridges +
                    8192;

        /* passthroughLimit := max( 2 GiB * #PHBs,                       (c)
         *                          memory                               (d)
         *                          + memory * 1/512 * #PHBs + 8 MiB )   (e)
         *
         * (c) is the pre-DDW VFIO DMA window accounting. We're allowing 2 GiB
         * rather than 1 GiB
         *
         * (d) is the with-DDW (and memory pre-registration and related
         * features) DMA window accounting - assuming that we only account RAM
         * once, even if mapped to multiple PHBs
         *
         * (e) is the with-DDW userspace view and overhead for the 64-bit DMA
         * window. This is based a bit on expected guest behaviour, but there
         * really isn't a way to completely avoid that. We assume the guest
         * requests a 64-bit DMA window (per PHB) just big enough to map all
         * its RAM. 4 kiB page size gives the 1/512; it will be less with 64
         * kiB pages, less still if the guest is mapped with hugepages (unlike
         * the default 32-bit DMA window, DDW windows can use large IOMMU
         * pages). 8 MiB is for second and further level overheads, like (b) */
        passthroughLimit = MAX(2 * 1024 * 1024 * nPCIHostBridges,
                               memory +
                               memory / 512 * nPCIHostBridges + 8192);

        if (usesVFIO)
            memKB = baseLimit + passthroughLimit;
        else
            memKB = baseLimit;

        goto done;
    }

    /* For device passthrough using VFIO the guest memory and MMIO memory
     * regions need to be locked persistent in order to allow DMA.
     *
     * Currently the below limit is based on assumptions about the x86 platform.
     *
     * The chosen value of 1GiB below originates from x86 systems where it was
     * used as space reserved for the MMIO region for the whole system.
     *
     * On x86_64 systems the MMIO regions of the IOMMU mapped devices don't
     * count towards the locked memory limit since the memory is owned by the
     * device. Emulated devices though do count, but the regions are usually
     * small. Although it's not guaranteed that the limit will be enough for all
     * configurations it didn't pose a problem for now.
     *
     * http://www.redhat.com/archives/libvir-list/2015-November/msg00329.html
     *
     * Note that this may not be valid for all platforms.
     */
    for (i = 0; i < def->nhostdevs; i++) {
        virDomainHostdevSubsysPtr subsys = &def->hostdevs[i]->source.subsys;

        if (def->hostdevs[i]->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
            (subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV ||
             (subsys->type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
              subsys->u.pci.backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO))) {
            memKB = virDomainDefGetMemoryTotal(def) + 1024 * 1024;
            goto done;
        }
    }

 done:
    return memKB << 10;
}


/**
 * qemuDomainAdjustMaxMemLock:
 * @vm: domain
 *
 * Adjust the memory locking limit for the QEMU process associated to @vm, in
 * order to comply with VFIO or architecture requirements.
 *
 * The limit will not be changed unless doing so is needed; the first time
 * the limit is changed, the original (default) limit is stored in @vm and
 * that value will be restored if qemuDomainAdjustMaxMemLock() is called once
 * memory locking is no longer required.
 *
 * Returns: 0 on success, <0 on failure
 */
int
qemuDomainAdjustMaxMemLock(virDomainObjPtr vm)
{
    unsigned long long bytes = 0;
    int ret = -1;

    bytes = qemuDomainGetMemLockLimitBytes(vm->def);

    if (bytes) {
        /* If this is the first time adjusting the limit, save the current
         * value so that we can restore it once memory locking is no longer
         * required. Failing to obtain the current limit is not a critical
         * failure, it just means we'll be unable to lower it later */
        if (!vm->original_memlock) {
            if (virProcessGetMaxMemLock(vm->pid, &(vm->original_memlock)) < 0)
                vm->original_memlock = 0;
        }
    } else {
        /* Once memory locking is no longer required, we can restore the
         * original, usually very low, limit */
        bytes = vm->original_memlock;
        vm->original_memlock = 0;
    }

    /* Trying to set the memory locking limit to zero is a no-op */
    if (virProcessSetMaxMemLock(vm->pid, bytes) < 0)
        goto out;

    ret = 0;

 out:
     return ret;
}

/**
 * qemuDomainHasVcpuPids:
 * @vm: Domain object
 *
 * Returns true if we were able to successfully detect vCPU pids for the VM.
 */
bool
qemuDomainHasVcpuPids(virDomainObjPtr vm)
{
    size_t i;
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virDomainVcpuDefPtr vcpu;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);

        if (QEMU_DOMAIN_VCPU_PRIVATE(vcpu)->tid > 0)
            return true;
    }

    return false;
}


/**
 * qemuDomainGetVcpuPid:
 * @vm: domain object
 * @vcpu: cpu id
 *
 * Returns the vCPU pid. If @vcpu is offline or out of range 0 is returned.
 */
pid_t
qemuDomainGetVcpuPid(virDomainObjPtr vm,
                     unsigned int vcpuid)
{
    virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(vm->def, vcpuid);
    return QEMU_DOMAIN_VCPU_PRIVATE(vcpu)->tid;
}


/**
 * qemuDomainValidateVcpuInfo:
 *
 * Validates vcpu thread information. If vcpu thread IDs are reported by qemu,
 * this function validates that online vcpus have thread info present and
 * offline vcpus don't.
 *
 * Returns 0 on success -1 on error.
 */
int
qemuDomainValidateVcpuInfo(virDomainObjPtr vm)
{
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    size_t i;

    if (!qemuDomainHasVcpuPids(vm))
        return 0;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        if (vcpu->online && vcpupriv->tid == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("qemu didn't report thread id for vcpu '%zu'"), i);
            return -1;
        }

        if (!vcpu->online && vcpupriv->tid != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("qemu reported thread id for inactive vcpu '%zu'"),
                           i);
            return -1;
        }
    }

    return 0;
}


bool
qemuDomainSupportsNewVcpuHotplug(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    return virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_QUERY_HOTPLUGGABLE_CPUS);
}


/**
 * qemuDomainRefreshVcpuInfo:
 * @driver: qemu driver data
 * @vm: domain object
 * @asyncJob: current asynchronous job type
 * @state: refresh vcpu state
 *
 * Updates vCPU information private data of @vm. Due to historical reasons this
 * function returns success even if some data were not reported by qemu.
 *
 * If @state is true, the vcpu state is refreshed as reported by the monitor.
 *
 * Returns 0 on success and -1 on fatal error.
 */
int
qemuDomainRefreshVcpuInfo(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          int asyncJob,
                          bool state)
{
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    qemuMonitorCPUInfoPtr info = NULL;
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    size_t i;
    bool hotplug;
    bool fast;
    int rc;
    int ret = -1;

    hotplug = qemuDomainSupportsNewVcpuHotplug(vm);
    fast = virQEMUCapsGet(QEMU_DOMAIN_PRIVATE(vm)->qemuCaps,
                          QEMU_CAPS_QUERY_CPUS_FAST);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    rc = qemuMonitorGetCPUInfo(qemuDomainGetMonitor(vm), &info, maxvcpus,
                               hotplug, fast);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (rc < 0)
        goto cleanup;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        /*
         * Current QEMU *can* report info about host threads mapped
         * to vCPUs, but it is not in a manner we can correctly
         * deal with. The TCG CPU emulation does have a separate vCPU
         * thread, but it runs every vCPU in that same thread. So it
         * is impossible to setup different affinity per thread.
         *
         * What's more the 'query-cpus[-fast]' command returns bizarre
         * data for the threads. It gives the TCG thread for the
         * vCPU 0, but for vCPUs 1-> N, it actually replies with
         * the main process thread ID.
         *
         * The result is that when we try to set affinity for
         * vCPU 1, it will actually change the affinity of the
         * emulator thread :-( When you try to set affinity for
         * vCPUs 2, 3.... it will fail if the affinity was
         * different from vCPU 1.
         *
         * We *could* allow vcpu pinning with TCG, if we made the
         * restriction that all vCPUs had the same mask. This would
         * at least let us separate emulator from vCPUs threads, as
         * we do for KVM. It would need some changes to our cgroups
         * CPU layout though, and error reporting for the config
         * restrictions.
         *
         * Just disable CPU pinning with TCG until someone wants
         * to try to do this hard work.
         */
        if (vm->def->virtType != VIR_DOMAIN_VIRT_QEMU)
            vcpupriv->tid = info[i].tid;

        vcpupriv->socket_id = info[i].socket_id;
        vcpupriv->core_id = info[i].core_id;
        vcpupriv->thread_id = info[i].thread_id;
        vcpupriv->node_id = info[i].node_id;
        vcpupriv->vcpus = info[i].vcpus;
        VIR_FREE(vcpupriv->type);
        VIR_STEAL_PTR(vcpupriv->type, info[i].type);
        VIR_FREE(vcpupriv->alias);
        VIR_STEAL_PTR(vcpupriv->alias, info[i].alias);
        vcpupriv->enable_id = info[i].id;
        vcpupriv->qemu_id = info[i].qemu_id;

        if (hotplug && state) {
            vcpu->online = info[i].online;
            if (info[i].hotpluggable)
                vcpu->hotpluggable = VIR_TRISTATE_BOOL_YES;
            else
                vcpu->hotpluggable = VIR_TRISTATE_BOOL_NO;
        }
    }

    ret = 0;

 cleanup:
    qemuMonitorCPUInfoFree(info, maxvcpus);
    return ret;
}

/**
 * qemuDomainGetVcpuHalted:
 * @vm: domain object
 * @vcpu: cpu id
 *
 * Returns the vCPU halted state.
  */
bool
qemuDomainGetVcpuHalted(virDomainObjPtr vm,
                        unsigned int vcpuid)
{
    virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(vm->def, vcpuid);
    return QEMU_DOMAIN_VCPU_PRIVATE(vcpu)->halted;
}

/**
 * qemuDomainRefreshVcpuHalted:
 * @driver: qemu driver data
 * @vm: domain object
 * @asyncJob: current asynchronous job type
 *
 * Updates vCPU halted state in the private data of @vm.
 *
 * Returns 0 on success and -1 on error
 */
int
qemuDomainRefreshVcpuHalted(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            int asyncJob)
{
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virBitmapPtr haltedmap = NULL;
    size_t i;
    int ret = -1;
    bool fast;

    /* Not supported currently for TCG, see qemuDomainRefreshVcpuInfo */
    if (vm->def->virtType == VIR_DOMAIN_VIRT_QEMU)
        return 0;

    /* The halted state is interresting only on s390(x). On other platforms
     * the data would be stale at the time when it would be used.
     * Calling qemuMonitorGetCpuHalted() can adversely affect the running
     * VM's performance unless QEMU supports query-cpus-fast.
     */
    if (!ARCH_IS_S390(vm->def->os.arch) ||
        !virQEMUCapsGet(QEMU_DOMAIN_PRIVATE(vm)->qemuCaps,
                        QEMU_CAPS_QUERY_CPUS_FAST))
        return 0;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    fast = virQEMUCapsGet(QEMU_DOMAIN_PRIVATE(vm)->qemuCaps,
                          QEMU_CAPS_QUERY_CPUS_FAST);
    haltedmap = qemuMonitorGetCpuHalted(qemuDomainGetMonitor(vm), maxvcpus,
                                        fast);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 || !haltedmap)
        goto cleanup;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);
        vcpupriv->halted = virTristateBoolFromBool(virBitmapIsBitSet(haltedmap,
                                                                     vcpupriv->qemu_id));
    }

    ret = 0;

 cleanup:
    virBitmapFree(haltedmap);
    return ret;
}

bool
qemuDomainSupportsNicdev(virDomainDefPtr def,
                         virDomainNetDefPtr net)
{
    /* non-virtio ARM nics require legacy -net nic */
    if (((def->os.arch == VIR_ARCH_ARMV7L) ||
        (def->os.arch == VIR_ARCH_AARCH64)) &&
        net->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO &&
        net->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI)
        return false;

    return true;
}

bool
qemuDomainNetSupportsMTU(virDomainNetType type)
{
    switch (type) {
    case VIR_DOMAIN_NET_TYPE_NETWORK:
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        return true;
    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_LAST:
        break;
    }
    return false;
}


virDomainDiskDefPtr
qemuDomainDiskByName(virDomainDefPtr def,
                     const char *name)
{
    virDomainDiskDefPtr ret;

    if (!(ret = virDomainDiskByName(def, name, true))) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("No device found for specified path"));
        return NULL;
    }

    return ret;
}


/**
 * qemuDomainDefValidateDiskLunSource:
 * @src: disk source struct
 *
 * Validate whether the disk source is valid for disk device='lun'.
 *
 * Returns 0 if the configuration is valid -1 and a libvirt error if the soure
 * is invalid.
 */
int
qemuDomainDefValidateDiskLunSource(const virStorageSource *src)
{
    if (virStorageSourceGetActualType(src) == VIR_STORAGE_TYPE_NETWORK) {
        if (src->protocol != VIR_STORAGE_NET_PROTOCOL_ISCSI) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("disk device='lun' is not supported "
                             "for protocol='%s'"),
                           virStorageNetProtocolTypeToString(src->protocol));
            return -1;
        }
    } else if (!virStorageSourceIsBlockLocal(src)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("disk device='lun' is only valid for block "
                         "type disk source"));
        return -1;
    }

    return 0;
}


int
qemuDomainPrepareChannel(virDomainChrDefPtr channel,
                         const char *domainChannelTargetDir)
{
    if (channel->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO ||
        channel->source->type != VIR_DOMAIN_CHR_TYPE_UNIX ||
        channel->source->data.nix.path)
        return 0;

    if (channel->target.name) {
        if (virAsprintf(&channel->source->data.nix.path,
                        "%s/%s", domainChannelTargetDir,
                        channel->target.name) < 0)
            return -1;
    } else {
        /* Generate a unique name */
        if (virAsprintf(&channel->source->data.nix.path,
                        "%s/vioser-%02d-%02d-%02d.sock",
                        domainChannelTargetDir,
                        channel->info.addr.vioserial.controller,
                        channel->info.addr.vioserial.bus,
                        channel->info.addr.vioserial.port) < 0)
            return -1;
    }

    return 0;
}


/* qemuDomainPrepareChardevSourceTLS:
 * @source: pointer to host interface data for char devices
 * @cfg: driver configuration
 *
 * Updates host interface TLS encryption setting based on qemu.conf
 * for char devices.  This will be presented as "tls='yes|no'" in
 * live XML of a guest.
 */
void
qemuDomainPrepareChardevSourceTLS(virDomainChrSourceDefPtr source,
                                  virQEMUDriverConfigPtr cfg)
{
    if (source->type == VIR_DOMAIN_CHR_TYPE_TCP) {
        if (source->data.tcp.haveTLS == VIR_TRISTATE_BOOL_ABSENT) {
            if (cfg->chardevTLS)
                source->data.tcp.haveTLS = VIR_TRISTATE_BOOL_YES;
            else
                source->data.tcp.haveTLS = VIR_TRISTATE_BOOL_NO;
            source->data.tcp.tlsFromConfig = true;
        }
    }
}


/* qemuDomainPrepareChardevSource:
 * @def: live domain definition
 * @cfg: driver configuration
 *
 * Iterate through all devices that use virDomainChrSourceDefPtr as host
 * interface part.
 */
void
qemuDomainPrepareChardevSource(virDomainDefPtr def,
                               virQEMUDriverConfigPtr cfg)
{
    size_t i;

    for (i = 0; i < def->nserials; i++)
        qemuDomainPrepareChardevSourceTLS(def->serials[i]->source, cfg);

    for (i = 0; i < def->nparallels; i++)
        qemuDomainPrepareChardevSourceTLS(def->parallels[i]->source, cfg);

    for (i = 0; i < def->nchannels; i++)
        qemuDomainPrepareChardevSourceTLS(def->channels[i]->source, cfg);

    for (i = 0; i < def->nconsoles; i++)
        qemuDomainPrepareChardevSourceTLS(def->consoles[i]->source, cfg);

    for (i = 0; i < def->nrngs; i++)
        if (def->rngs[i]->backend == VIR_DOMAIN_RNG_BACKEND_EGD)
            qemuDomainPrepareChardevSourceTLS(def->rngs[i]->source.chardev, cfg);

    for (i = 0; i < def->nsmartcards; i++)
        if (def->smartcards[i]->type == VIR_DOMAIN_SMARTCARD_TYPE_PASSTHROUGH)
            qemuDomainPrepareChardevSourceTLS(def->smartcards[i]->data.passthru,
                                              cfg);

    for (i = 0; i < def->nredirdevs; i++)
        qemuDomainPrepareChardevSourceTLS(def->redirdevs[i]->source, cfg);
}


/* qemuProcessPrepareDiskSourceTLS:
 * @source: pointer to host interface data for disk device
 * @cfg: driver configuration
 *
 * Updates host interface TLS encryption setting based on qemu.conf
 * for disk devices.  This will be presented as "tls='yes|no'" in
 * live XML of a guest.
 *
 * Returns 0 on success, -1 on bad config/failure
 */
static int
qemuDomainPrepareDiskSourceTLS(virStorageSourcePtr src,
                               virQEMUDriverConfigPtr cfg)
{
    virStorageSourcePtr next;

    for (next = src; virStorageSourceIsBacking(next); next = next->backingStore) {
        /* VxHS uses only client certificates and thus has no need for
         * the server-key.pem nor a secret that could be used to decrypt
         * the it, so no need to add a secinfo for a secret UUID. */
        if (next->type == VIR_STORAGE_TYPE_NETWORK &&
            next->protocol == VIR_STORAGE_NET_PROTOCOL_VXHS) {

            if (next->haveTLS == VIR_TRISTATE_BOOL_ABSENT) {
                if (cfg->vxhsTLS)
                    next->haveTLS = VIR_TRISTATE_BOOL_YES;
                else
                    next->haveTLS = VIR_TRISTATE_BOOL_NO;
                next->tlsFromConfig = true;
            }

            if (next->haveTLS == VIR_TRISTATE_BOOL_YES) {
                /* Grab the vxhsTLSx509certdir and set the verify/listen values.
                 * NB: tlsAlias filled in during qemuDomainGetTLSObjects. */
                if (VIR_STRDUP(next->tlsCertdir, cfg->vxhsTLSx509certdir) < 0)
                    return -1;

                next->tlsVerify = true;
            }
        }
    }

    return 0;
}


int
qemuDomainPrepareShmemChardev(virDomainShmemDefPtr shmem)
{
    if (!shmem->server.enabled ||
        shmem->server.chr.data.nix.path)
        return 0;

    return virAsprintf(&shmem->server.chr.data.nix.path,
                       "/var/lib/libvirt/shmem-%s-sock",
                       shmem->name);
}


/**
 * qemuDomainVcpuHotplugIsInOrder:
 * @def: domain definition
 *
 * Returns true if online vcpus were added in order (clustered behind vcpu0
 * with increasing order).
 */
bool
qemuDomainVcpuHotplugIsInOrder(virDomainDefPtr def)
{
    size_t maxvcpus = virDomainDefGetVcpusMax(def);
    virDomainVcpuDefPtr vcpu;
    unsigned int prevorder = 0;
    size_t seenonlinevcpus = 0;
    size_t i;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);

        if (!vcpu->online)
            break;

        if (vcpu->order < prevorder)
            break;

        if (vcpu->order > prevorder)
            prevorder = vcpu->order;

        seenonlinevcpus++;
    }

    return seenonlinevcpus == virDomainDefGetVcpus(def);
}


/**
 * qemuDomainVcpuPersistOrder:
 * @def: domain definition
 *
 * Saves the order of vcpus detected from qemu to the domain definition.
 * The private data note the order only for the entry describing the
 * hotpluggable entity. This function copies the order into the definition part
 * of all sub entities.
 */
void
qemuDomainVcpuPersistOrder(virDomainDefPtr def)
{
    size_t maxvcpus = virDomainDefGetVcpusMax(def);
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    unsigned int prevorder = 0;
    size_t i;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(def, i);
        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        if (!vcpu->online) {
            vcpu->order = 0;
        } else {
            if (vcpupriv->enable_id != 0)
                prevorder = vcpupriv->enable_id;

            vcpu->order = prevorder;
        }
    }
}


int
qemuDomainCheckMonitor(virQEMUDriverPtr driver,
                       virDomainObjPtr vm,
                       qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    ret = qemuMonitorCheck(priv->mon);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    return ret;
}


bool
qemuDomainSupportsVideoVga(virDomainVideoDefPtr video,
                           virQEMUCapsPtr qemuCaps)
{
    if (video->type == VIR_DOMAIN_VIDEO_TYPE_VIRTIO &&
        !virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_VIRTIO_VGA))
        return false;

    return true;
}


/**
 * qemuDomainGetHostdevPath:
 * @def: domain definition
 * @dev: host device definition
 * @teardown: true if device will be removed
 * @npaths: number of items in @path and @perms arrays
 * @path: resulting path to @dev
 * @perms: Optional pointer to VIR_CGROUP_DEVICE_* perms
 *
 * For given device @dev fetch its host path and store it at
 * @path. If a device requires other paths to be present/allowed
 * they are stored in the @path array after the actual path.
 * Optionally, caller can get @perms on the path (e.g. rw/ro).
 *
 * The caller is responsible for freeing the memory.
 *
 * Returns 0 on success, -1 otherwise.
 */
int
qemuDomainGetHostdevPath(virDomainDefPtr def,
                         virDomainHostdevDefPtr dev,
                         bool teardown,
                         size_t *npaths,
                         char ***path,
                         int **perms)
{
    int ret = -1;
    virDomainHostdevSubsysUSBPtr usbsrc = &dev->source.subsys.u.usb;
    virDomainHostdevSubsysPCIPtr pcisrc = &dev->source.subsys.u.pci;
    virDomainHostdevSubsysSCSIPtr scsisrc = &dev->source.subsys.u.scsi;
    virDomainHostdevSubsysSCSIVHostPtr hostsrc = &dev->source.subsys.u.scsi_host;
    virDomainHostdevSubsysMediatedDevPtr mdevsrc = &dev->source.subsys.u.mdev;
    virPCIDevicePtr pci = NULL;
    virUSBDevicePtr usb = NULL;
    virSCSIDevicePtr scsi = NULL;
    virSCSIVHostDevicePtr host = NULL;
    char *tmpPath = NULL;
    bool freeTmpPath = false;
    bool includeVFIO = false;
    char **tmpPaths = NULL;
    int *tmpPerms = NULL;
    size_t i, tmpNpaths = 0;
    int perm = 0;

    *npaths = 0;

    switch ((virDomainHostdevMode) dev->mode) {
    case VIR_DOMAIN_HOSTDEV_MODE_SUBSYS:
        switch ((virDomainHostdevSubsysType)dev->source.subsys.type) {
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI:
            if (pcisrc->backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO) {
                pci = virPCIDeviceNew(pcisrc->addr.domain,
                                      pcisrc->addr.bus,
                                      pcisrc->addr.slot,
                                      pcisrc->addr.function);
                if (!pci)
                    goto cleanup;

                if (!(tmpPath = virPCIDeviceGetIOMMUGroupDev(pci)))
                    goto cleanup;
                freeTmpPath = true;

                perm = VIR_CGROUP_DEVICE_RW;
                if (teardown) {
                    size_t nvfios = 0;
                    for (i = 0; i < def->nhostdevs; i++) {
                        virDomainHostdevDefPtr tmp = def->hostdevs[i];
                        if (tmp->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
                            tmp->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI &&
                            tmp->source.subsys.u.pci.backend == VIR_DOMAIN_HOSTDEV_PCI_BACKEND_VFIO)
                            nvfios++;
                    }

                    if (nvfios == 0)
                        includeVFIO = true;
                } else {
                    includeVFIO = true;
                }
            }
            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_USB:
            if (dev->missing)
                break;
            usb = virUSBDeviceNew(usbsrc->bus,
                                  usbsrc->device,
                                  NULL);
            if (!usb)
                goto cleanup;

            if (!(tmpPath = (char *)virUSBDeviceGetPath(usb)))
                goto cleanup;
            perm = VIR_CGROUP_DEVICE_RW;
            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI:
            if (scsisrc->protocol == VIR_DOMAIN_HOSTDEV_SCSI_PROTOCOL_TYPE_ISCSI) {
                virDomainHostdevSubsysSCSIiSCSIPtr iscsisrc = &scsisrc->u.iscsi;
                /* Follow qemuSetupDiskCgroup() and qemuSetImageCgroupInternal()
                 * which does nothing for non local storage
                 */
                VIR_DEBUG("Not updating /dev for hostdev iSCSI path '%s'", iscsisrc->src->path);
            } else {
                virDomainHostdevSubsysSCSIHostPtr scsihostsrc = &scsisrc->u.host;
                scsi = virSCSIDeviceNew(NULL,
                                        scsihostsrc->adapter,
                                        scsihostsrc->bus,
                                        scsihostsrc->target,
                                        scsihostsrc->unit,
                                        dev->readonly,
                                        dev->shareable);

                if (!scsi)
                    goto cleanup;

                if (!(tmpPath = (char *)virSCSIDeviceGetPath(scsi)))
                    goto cleanup;
                perm = virSCSIDeviceGetReadonly(scsi) ?
                    VIR_CGROUP_DEVICE_READ : VIR_CGROUP_DEVICE_RW;
            }
            break;

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI_HOST: {
            if (hostsrc->protocol ==
                VIR_DOMAIN_HOSTDEV_SUBSYS_SCSI_HOST_PROTOCOL_TYPE_VHOST) {
                if (!(host = virSCSIVHostDeviceNew(hostsrc->wwpn)))
                    goto cleanup;

                if (!(tmpPath = (char *)virSCSIVHostDeviceGetPath(host)))
                    goto cleanup;
                perm = VIR_CGROUP_DEVICE_RW;
            }
            break;
        }

        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_MDEV:
            if (!(tmpPath = virMediatedDeviceGetIOMMUGroupDev(mdevsrc->uuidstr)))
                goto cleanup;

            freeTmpPath = true;
            includeVFIO = true;
            perm = VIR_CGROUP_DEVICE_RW;
            break;
        case VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_LAST:
            break;
        }
        break;

    case VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES:
    case VIR_DOMAIN_HOSTDEV_MODE_LAST:
        /* nada */
        break;
    }

    if (tmpPath) {
        size_t toAlloc = 1;

        if (includeVFIO)
            toAlloc = 2;

        if (VIR_ALLOC_N(tmpPaths, toAlloc) < 0 ||
            VIR_ALLOC_N(tmpPerms, toAlloc) < 0 ||
            VIR_STRDUP(tmpPaths[0], tmpPath) < 0)
            goto cleanup;
        tmpNpaths = toAlloc;
        tmpPerms[0] = perm;

        if (includeVFIO) {
            if (VIR_STRDUP(tmpPaths[1], DEV_VFIO) < 0)
                goto cleanup;
            tmpPerms[1] = VIR_CGROUP_DEVICE_RW;
        }
    }

    *npaths = tmpNpaths;
    tmpNpaths = 0;
    *path = tmpPaths;
    tmpPaths = NULL;
    if (perms) {
        *perms = tmpPerms;
        tmpPerms = NULL;
    }
    ret = 0;
 cleanup:
    for (i = 0; i < tmpNpaths; i++)
        VIR_FREE(tmpPaths[i]);
    VIR_FREE(tmpPaths);
    VIR_FREE(tmpPerms);
    virPCIDeviceFree(pci);
    virUSBDeviceFree(usb);
    virSCSIDeviceFree(scsi);
    virSCSIVHostDeviceFree(host);
    if (freeTmpPath)
        VIR_FREE(tmpPath);
    return ret;
}


/**
 * qemuDomainGetPreservedMountPath:
 * @cfg: driver configuration data
 * @vm: domain object
 * @mountpoint: mount point path to convert
 *
 * For given @mountpoint return new path where the mount point
 * should be moved temporarily whilst building the namespace.
 *
 * Returns: allocated string on success which the caller must free,
 *          NULL on failure.
 */
static char *
qemuDomainGetPreservedMountPath(virQEMUDriverConfigPtr cfg,
                                virDomainObjPtr vm,
                                const char *mountpoint)
{
    char *path = NULL;
    char *tmp;
    const char *suffix = mountpoint + strlen(DEVPREFIX);
    char *domname = virDomainDefGetShortName(vm->def);
    size_t off;

    if (!domname)
        return NULL;

    if (STREQ(mountpoint, "/dev"))
        suffix = "dev";

    if (virAsprintf(&path, "%s/%s.%s",
                    cfg->stateDir, domname, suffix) < 0)
        goto cleanup;

    /* Now consider that @mountpoint is "/dev/blah/blah2".
     * @suffix then points to "blah/blah2". However, caller
     * expects all the @paths to be the same depth. The
     * caller doesn't always do `mkdir -p` but sometimes bare
     * `touch`. Therefore fix all the suffixes. */
    off = strlen(path) - strlen(suffix);

    tmp = path + off;
    while (*tmp) {
        if (*tmp == '/')
            *tmp = '.';
        tmp++;
    }

 cleanup:
    VIR_FREE(domname);
    return path;
}


/**
 * qemuDomainGetPreservedMounts:
 *
 * Process list of mounted filesystems and:
 * a) save all FSs mounted under /dev to @devPath
 * b) generate backup path for all the entries in a)
 *
 * Any of the return pointers can be NULL.
 *
 * Returns 0 on success, -1 otherwise (with error reported)
 */
static int
qemuDomainGetPreservedMounts(virQEMUDriverConfigPtr cfg,
                             virDomainObjPtr vm,
                             char ***devPath,
                             char ***devSavePath,
                             size_t *ndevPath)
{
    char **paths = NULL, **mounts = NULL;
    size_t i, j, nmounts;

    if (virFileGetMountSubtree(PROC_MOUNTS, "/dev",
                               &mounts, &nmounts) < 0)
        goto error;

    if (!nmounts) {
        if (ndevPath)
            *ndevPath = 0;
        return 0;
    }

    /* There can be nested mount points. For instance
     * /dev/shm/blah can be a mount point and /dev/shm too. It
     * doesn't make much sense to return the former path because
     * caller preserves the latter (and with that the former
     * too). Therefore prune nested mount points.
     * NB mounts[0] is "/dev". Should we start the outer loop
     * from the beginning of the array all we'd be left with is
     * just the first element. Think about it.
     */
    for (i = 1; i < nmounts; i++) {
        j = i + 1;
        while (j < nmounts) {
            char *c = STRSKIP(mounts[j], mounts[i]);

            if (c && (*c == '/' || *c == '\0')) {
                VIR_DEBUG("Dropping path %s because of %s", mounts[j], mounts[i]);
                VIR_DELETE_ELEMENT(mounts, j, nmounts);
            } else {
                j++;
            }
        }
    }

    if (VIR_ALLOC_N(paths, nmounts) < 0)
        goto error;

    for (i = 0; i < nmounts; i++) {
        if (!(paths[i] = qemuDomainGetPreservedMountPath(cfg, vm, mounts[i])))
            goto error;
    }

    if (devPath)
        *devPath = mounts;
    else
        virStringListFreeCount(mounts, nmounts);

    if (devSavePath)
        *devSavePath = paths;
    else
        virStringListFreeCount(paths, nmounts);

    if (ndevPath)
        *ndevPath = nmounts;

    return 0;

 error:
    virStringListFreeCount(mounts, nmounts);
    virStringListFreeCount(paths, nmounts);
    return -1;
}


struct qemuDomainCreateDeviceData {
    const char *path;     /* Path to temp new /dev location */
    char * const *devMountsPath;
    size_t ndevMountsPath;
};


static int
qemuDomainCreateDeviceRecursive(const char *device,
                                const struct qemuDomainCreateDeviceData *data,
                                bool allow_noent,
                                unsigned int ttl)
{
    char *devicePath = NULL;
    char *target = NULL;
    struct stat sb;
    int ret = -1;
    bool isLink = false;
    bool isDev = false;
    bool isReg = false;
    bool isDir = false;
    bool create = false;
#ifdef WITH_SELINUX
    char *tcon = NULL;
#endif

    if (!ttl) {
        virReportSystemError(ELOOP,
                             _("Too many levels of symbolic links: %s"),
                             device);
        return ret;
    }

    if (lstat(device, &sb) < 0) {
        if (errno == ENOENT && allow_noent) {
            /* Ignore non-existent device. */
            return 0;
        }
        virReportSystemError(errno, _("Unable to stat %s"), device);
        return ret;
    }

    isLink = S_ISLNK(sb.st_mode);
    isDev = S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode);
    isReg = S_ISREG(sb.st_mode) || S_ISFIFO(sb.st_mode) || S_ISSOCK(sb.st_mode);
    isDir = S_ISDIR(sb.st_mode);

    /* Here, @device might be whatever path in the system. We
     * should create the path in the namespace iff it's "/dev"
     * prefixed. However, if it is a symlink, we need to traverse
     * it too (it might point to something in "/dev"). Just
     * consider:
     *
     *   /var/sym1 -> /var/sym2 -> /dev/sda  (because users can)
     *
     * This means, "/var/sym1" is not created (it's shared with
     * the parent namespace), nor "/var/sym2", but "/dev/sda".
     *
     * TODO Remove all `.' and `..' from the @device path.
     * Otherwise we might get fooled with `/dev/../var/my_image'.
     * For now, lets hope callers play nice.
     */
    if (STRPREFIX(device, DEVPREFIX)) {
        size_t i;

        for (i = 0; i < data->ndevMountsPath; i++) {
            if (STREQ(data->devMountsPath[i], "/dev"))
                continue;
            if (STRPREFIX(device, data->devMountsPath[i]))
                break;
        }

        if (i == data->ndevMountsPath) {
            /* Okay, @device is in /dev but not in any mount point under /dev.
             * Create it. */
            if (virAsprintf(&devicePath, "%s/%s",
                            data->path, device + strlen(DEVPREFIX)) < 0)
                goto cleanup;

            if (virFileMakeParentPath(devicePath) < 0) {
                virReportSystemError(errno,
                                     _("Unable to create %s"),
                                     devicePath);
                goto cleanup;
            }
            VIR_DEBUG("Creating dev %s", device);
            create = true;
        } else {
            VIR_DEBUG("Skipping dev %s because of %s mount point",
                      device, data->devMountsPath[i]);
        }
    }

    if (isLink) {
        /* We are dealing with a symlink. Create a dangling symlink and descend
         * down one level which hopefully creates the symlink's target. */
        if (virFileReadLink(device, &target) < 0) {
            virReportSystemError(errno,
                                 _("unable to resolve symlink %s"),
                                 device);
            goto cleanup;
        }

        if (create &&
            symlink(target, devicePath) < 0) {
            if (errno == EEXIST) {
                ret = 0;
            } else {
                virReportSystemError(errno,
                                     _("unable to create symlink %s"),
                                     devicePath);
            }
            goto cleanup;
        }

        /* Tricky part. If the target starts with a slash then we need to take
         * it as it is. Otherwise we need to replace the last component in the
         * original path with the link target:
         * /dev/rtc -> rtc0 (want /dev/rtc0)
         * /dev/disk/by-id/ata-SanDisk_SDSSDXPS480G_161101402485 -> ../../sda
         *   (want /dev/disk/by-id/../../sda)
         * /dev/stdout -> /proc/self/fd/1 (no change needed)
         */
        if (IS_RELATIVE_FILE_NAME(target)) {
            char *c = NULL, *tmp = NULL, *devTmp = NULL;

            if (VIR_STRDUP(devTmp, device) < 0)
                goto cleanup;

            if ((c = strrchr(devTmp, '/')))
                *(c + 1) = '\0';

            if (virAsprintf(&tmp, "%s%s", devTmp, target) < 0) {
                VIR_FREE(devTmp);
                goto cleanup;
            }
            VIR_FREE(devTmp);
            VIR_FREE(target);
            target = tmp;
            tmp = NULL;
        }

        if (qemuDomainCreateDeviceRecursive(target, data,
                                            allow_noent, ttl - 1) < 0)
            goto cleanup;
    } else if (isDev) {
        if (create &&
            mknod(devicePath, sb.st_mode, sb.st_rdev) < 0) {
            if (errno == EEXIST) {
                ret = 0;
            } else {
                virReportSystemError(errno,
                                     _("Failed to make device %s"),
                                     devicePath);
            }
            goto cleanup;
        }
    } else if (isReg) {
        if (create &&
            virFileTouch(devicePath, sb.st_mode) < 0)
            goto cleanup;
        /* Just create the file here so that code below sets
         * proper owner and mode. Bind mount only after that. */
    } else if (isDir) {
        if (create &&
            virFileMakePathWithMode(devicePath, sb.st_mode) < 0)
            goto cleanup;
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("unsupported device type %s 0%o"),
                       device, sb.st_mode);
        goto cleanup;
    }

    if (!create) {
        ret = 0;
        goto cleanup;
    }

    if (lchown(devicePath, sb.st_uid, sb.st_gid) < 0) {
        virReportSystemError(errno,
                             _("Failed to chown device %s"),
                             devicePath);
        goto cleanup;
    }

    /* Symlinks don't have mode */
    if (!isLink &&
        chmod(devicePath, sb.st_mode) < 0) {
        virReportSystemError(errno,
                             _("Failed to set permissions for device %s"),
                             devicePath);
        goto cleanup;
    }

    /* Symlinks don't have ACLs. */
    if (!isLink &&
        virFileCopyACLs(device, devicePath) < 0 &&
        errno != ENOTSUP) {
        virReportSystemError(errno,
                             _("Failed to copy ACLs on device %s"),
                             devicePath);
        goto cleanup;
    }

#ifdef WITH_SELINUX
    if (lgetfilecon_raw(device, &tcon) < 0 &&
        (errno != ENOTSUP && errno != ENODATA)) {
        virReportSystemError(errno,
                             _("Unable to get SELinux label from %s"),
                             device);
        goto cleanup;
    }

    if (tcon &&
        lsetfilecon_raw(devicePath, (VIR_SELINUX_CTX_CONST char *)tcon) < 0) {
        VIR_WARNINGS_NO_WLOGICALOP_EQUAL_EXPR
        if (errno != EOPNOTSUPP && errno != ENOTSUP) {
        VIR_WARNINGS_RESET
            virReportSystemError(errno,
                                 _("Unable to set SELinux label on %s"),
                                 devicePath);
            goto cleanup;
        }
    }
#endif

    /* Finish mount process started earlier. */
    if ((isReg || isDir) &&
        virFileBindMountDevice(device, devicePath) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(target);
    VIR_FREE(devicePath);
#ifdef WITH_SELINUX
    freecon(tcon);
#endif
    return ret;
}


static int
qemuDomainCreateDevice(const char *device,
                       const struct qemuDomainCreateDeviceData *data,
                       bool allow_noent)
{
    long symloop_max = sysconf(_SC_SYMLOOP_MAX);

    return qemuDomainCreateDeviceRecursive(device, data,
                                           allow_noent, symloop_max);
}


static int
qemuDomainPopulateDevices(virQEMUDriverConfigPtr cfg,
                          virDomainObjPtr vm ATTRIBUTE_UNUSED,
                          const struct qemuDomainCreateDeviceData *data)
{
    const char *const *devices = (const char *const *) cfg->cgroupDeviceACL;
    size_t i;
    int ret = -1;

    if (!devices)
        devices = defaultDeviceACL;

    for (i = 0; devices[i]; i++) {
        if (qemuDomainCreateDevice(devices[i], data, true) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    return ret;
}


static int
qemuDomainSetupDev(virQEMUDriverConfigPtr cfg,
                   virSecurityManagerPtr mgr,
                   virDomainObjPtr vm,
                   const struct qemuDomainCreateDeviceData *data)
{
    char *mount_options = NULL;
    char *opts = NULL;
    int ret = -1;

    VIR_DEBUG("Setting up /dev/ for domain %s", vm->def->name);

    mount_options = qemuSecurityGetMountOptions(mgr, vm->def);

    if (!mount_options &&
        VIR_STRDUP(mount_options, "") < 0)
        goto cleanup;

    /*
     * tmpfs is limited to 64kb, since we only have device nodes in there
     * and don't want to DOS the entire OS RAM usage
     */
    if (virAsprintf(&opts,
                    "mode=755,size=65536%s", mount_options) < 0)
        goto cleanup;

    if (virFileSetupDev(data->path, opts) < 0)
        goto cleanup;

    if (qemuDomainPopulateDevices(cfg, vm, data) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(opts);
    VIR_FREE(mount_options);
    return ret;
}


static int
qemuDomainSetupDisk(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                    virDomainDiskDefPtr disk,
                    const struct qemuDomainCreateDeviceData *data)
{
    virStorageSourcePtr next;
    char *dst = NULL;
    int ret = -1;

    for (next = disk->src; virStorageSourceIsBacking(next); next = next->backingStore) {
        if (!next->path || !virStorageSourceIsLocalStorage(next)) {
            /* Not creating device. Just continue. */
            continue;
        }

        if (qemuDomainCreateDevice(next->path, data, false) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(dst);
    return ret;
}


static int
qemuDomainSetupAllDisks(virQEMUDriverConfigPtr cfg,
                        virDomainObjPtr vm,
                        const struct qemuDomainCreateDeviceData *data)
{
    size_t i;
    VIR_DEBUG("Setting up disks");

    for (i = 0; i < vm->def->ndisks; i++) {
        if (qemuDomainSetupDisk(cfg,
                                vm->def->disks[i],
                                data) < 0)
            return -1;
    }

    VIR_DEBUG("Setup all disks");
    return 0;
}


static int
qemuDomainSetupHostdev(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                       virDomainHostdevDefPtr dev,
                       const struct qemuDomainCreateDeviceData *data)
{
    int ret = -1;
    char **path = NULL;
    size_t i, npaths = 0;

    if (qemuDomainGetHostdevPath(NULL, dev, false, &npaths, &path, NULL) < 0)
        goto cleanup;

    for (i = 0; i < npaths; i++) {
        if (qemuDomainCreateDevice(path[i], data, false) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    for (i = 0; i < npaths; i++)
        VIR_FREE(path[i]);
    VIR_FREE(path);
    return ret;
}


static int
qemuDomainSetupAllHostdevs(virQEMUDriverConfigPtr cfg,
                           virDomainObjPtr vm,
                           const struct qemuDomainCreateDeviceData *data)
{
    size_t i;

    VIR_DEBUG("Setting up hostdevs");
    for (i = 0; i < vm->def->nhostdevs; i++) {
        if (qemuDomainSetupHostdev(cfg,
                                   vm->def->hostdevs[i],
                                   data) < 0)
            return -1;
    }
    VIR_DEBUG("Setup all hostdevs");
    return 0;
}


static int
qemuDomainSetupMemory(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                      virDomainMemoryDefPtr mem,
                      const struct qemuDomainCreateDeviceData *data)
{
    if (mem->model != VIR_DOMAIN_MEMORY_MODEL_NVDIMM)
        return 0;

    return qemuDomainCreateDevice(mem->nvdimmPath, data, false);
}


static int
qemuDomainSetupAllMemories(virQEMUDriverConfigPtr cfg,
                           virDomainObjPtr vm,
                           const struct qemuDomainCreateDeviceData *data)
{
    size_t i;

    VIR_DEBUG("Setting up memories");
    for (i = 0; i < vm->def->nmems; i++) {
        if (qemuDomainSetupMemory(cfg,
                                  vm->def->mems[i],
                                  data) < 0)
            return -1;
    }
    VIR_DEBUG("Setup all memories");
    return 0;
}


static int
qemuDomainSetupChardev(virDomainDefPtr def ATTRIBUTE_UNUSED,
                       virDomainChrDefPtr dev,
                       void *opaque)
{
    const struct qemuDomainCreateDeviceData *data = opaque;
    const char *path = NULL;

    if (!(path = virDomainChrSourceDefGetPath(dev->source)))
        return 0;

    /* Socket created by qemu. It doesn't exist upfront. */
    if (dev->source->type == VIR_DOMAIN_CHR_TYPE_UNIX &&
        dev->source->data.nix.listen)
        return 0;

    return qemuDomainCreateDevice(path, data, true);
}


static int
qemuDomainSetupAllChardevs(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm,
                           const struct qemuDomainCreateDeviceData *data)
{
    VIR_DEBUG("Setting up chardevs");

    if (virDomainChrDefForeach(vm->def,
                               true,
                               qemuDomainSetupChardev,
                               (void *)data) < 0)
        return -1;

    VIR_DEBUG("Setup all chardevs");
    return 0;
}


static int
qemuDomainSetupTPM(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                   virDomainObjPtr vm,
                   const struct qemuDomainCreateDeviceData *data)
{
    virDomainTPMDefPtr dev = vm->def->tpm;

    if (!dev)
        return 0;

    VIR_DEBUG("Setting up TPM");

    switch (dev->type) {
    case VIR_DOMAIN_TPM_TYPE_PASSTHROUGH:
        if (qemuDomainCreateDevice(dev->data.passthrough.source.data.file.path,
                                   data, false) < 0)
            return -1;
        break;

    case VIR_DOMAIN_TPM_TYPE_LAST:
        /* nada */
        break;
    }

    VIR_DEBUG("Setup TPM");
    return 0;
}


static int
qemuDomainSetupGraphics(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                        virDomainGraphicsDefPtr gfx,
                        const struct qemuDomainCreateDeviceData *data)
{
    const char *rendernode = gfx->data.spice.rendernode;

    if (gfx->type != VIR_DOMAIN_GRAPHICS_TYPE_SPICE ||
        gfx->data.spice.gl != VIR_TRISTATE_BOOL_YES ||
        !rendernode)
        return 0;

    return qemuDomainCreateDevice(rendernode, data, false);
}


static int
qemuDomainSetupAllGraphics(virQEMUDriverConfigPtr cfg,
                           virDomainObjPtr vm,
                           const struct qemuDomainCreateDeviceData *data)
{
    size_t i;

    VIR_DEBUG("Setting up graphics");
    for (i = 0; i < vm->def->ngraphics; i++) {
        if (qemuDomainSetupGraphics(cfg,
                                    vm->def->graphics[i],
                                    data) < 0)
            return -1;
    }

    VIR_DEBUG("Setup all graphics");
    return 0;
}


static int
qemuDomainSetupInput(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                     virDomainInputDefPtr input,
                     const struct qemuDomainCreateDeviceData *data)
{
    const char *path = virDomainInputDefGetPath(input);

    if (path && qemuDomainCreateDevice(path, data, false) < 0)
        return -1;

    return 0;
}


static int
qemuDomainSetupAllInputs(virQEMUDriverConfigPtr cfg,
                         virDomainObjPtr vm,
                         const struct qemuDomainCreateDeviceData *data)
{
    size_t i;

    VIR_DEBUG("Setting up inputs");
    for (i = 0; i < vm->def->ninputs; i++) {
        if (qemuDomainSetupInput(cfg,
                                 vm->def->inputs[i],
                                 data) < 0)
            return -1;
    }
    VIR_DEBUG("Setup all inputs");
    return 0;
}


static int
qemuDomainSetupRNG(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                   virDomainRNGDefPtr rng,
                   const struct qemuDomainCreateDeviceData *data)
{
    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        if (qemuDomainCreateDevice(rng->source.file, data, false) < 0)
            return -1;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        /* nada */
        break;
    }

    return 0;
}


static int
qemuDomainSetupAllRNGs(virQEMUDriverConfigPtr cfg,
                       virDomainObjPtr vm,
                       const struct qemuDomainCreateDeviceData *data)
{
    size_t i;

    VIR_DEBUG("Setting up RNGs");
    for (i = 0; i < vm->def->nrngs; i++) {
        if (qemuDomainSetupRNG(cfg,
                               vm->def->rngs[i],
                               data) < 0)
            return -1;
    }

    VIR_DEBUG("Setup all RNGs");
    return 0;
}


static int
qemuDomainSetupLoader(virQEMUDriverConfigPtr cfg ATTRIBUTE_UNUSED,
                      virDomainObjPtr vm,
                      const struct qemuDomainCreateDeviceData *data)
{
    virDomainLoaderDefPtr loader = vm->def->os.loader;
    int ret = -1;

    VIR_DEBUG("Setting up loader");

    if (loader) {
        switch ((virDomainLoader) loader->type) {
        case VIR_DOMAIN_LOADER_TYPE_ROM:
            if (qemuDomainCreateDevice(loader->path, data, false) < 0)
                goto cleanup;
            break;

        case VIR_DOMAIN_LOADER_TYPE_PFLASH:
            if (qemuDomainCreateDevice(loader->path, data, false) < 0)
                goto cleanup;

            if (loader->nvram &&
                qemuDomainCreateDevice(loader->nvram, data, false) < 0)
                goto cleanup;
            break;

        case VIR_DOMAIN_LOADER_TYPE_LAST:
            break;
        }
    }

    VIR_DEBUG("Setup loader");
    ret = 0;
 cleanup:
    return ret;
}


int
qemuDomainBuildNamespace(virQEMUDriverConfigPtr cfg,
                         virSecurityManagerPtr mgr,
                         virDomainObjPtr vm)
{
    struct qemuDomainCreateDeviceData data;
    char *devPath = NULL;
    char **devMountsPath = NULL, **devMountsSavePath = NULL;
    size_t ndevMountsPath = 0, i;
    int ret = -1;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT)) {
        ret = 0;
        goto cleanup;
    }

    if (qemuDomainGetPreservedMounts(cfg, vm,
                                     &devMountsPath, &devMountsSavePath,
                                     &ndevMountsPath) < 0)
        goto cleanup;

    for (i = 0; i < ndevMountsPath; i++) {
        if (STREQ(devMountsPath[i], "/dev")) {
            devPath = devMountsSavePath[i];
            break;
        }
    }

    if (!devPath) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to find any /dev mount"));
        goto cleanup;
    }

    data.path = devPath;
    data.devMountsPath = devMountsPath;
    data.ndevMountsPath = ndevMountsPath;

    if (virProcessSetupPrivateMountNS() < 0)
        goto cleanup;

    if (qemuDomainSetupDev(cfg, mgr, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllDisks(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllHostdevs(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllMemories(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllChardevs(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupTPM(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllGraphics(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllInputs(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupAllRNGs(cfg, vm, &data) < 0)
        goto cleanup;

    if (qemuDomainSetupLoader(cfg, vm, &data) < 0)
        goto cleanup;

    /* Save some mount points because we want to share them with the host */
    for (i = 0; i < ndevMountsPath; i++) {
        struct stat sb;

        if (devMountsSavePath[i] == devPath)
            continue;

        if (stat(devMountsPath[i], &sb) < 0) {
            virReportSystemError(errno,
                                 _("Unable to stat: %s"),
                                 devMountsPath[i]);
            goto cleanup;
        }

        /* At this point, devMountsPath is either:
         * a file (regular or special), or
         * a directory. */
        if ((S_ISDIR(sb.st_mode) && virFileMakePath(devMountsSavePath[i]) < 0) ||
            (!S_ISDIR(sb.st_mode) && virFileTouch(devMountsSavePath[i], sb.st_mode) < 0)) {
            virReportSystemError(errno,
                                 _("Failed to create %s"),
                                 devMountsSavePath[i]);
            goto cleanup;
        }

        if (virFileMoveMount(devMountsPath[i], devMountsSavePath[i]) < 0)
            goto cleanup;
    }

    if (virFileMoveMount(devPath, "/dev") < 0)
        goto cleanup;

    for (i = 0; i < ndevMountsPath; i++) {
        struct stat sb;

        if (devMountsSavePath[i] == devPath)
            continue;

        if (stat(devMountsSavePath[i], &sb) < 0) {
            virReportSystemError(errno,
                                 _("Unable to stat: %s"),
                                 devMountsSavePath[i]);
            goto cleanup;
        }

        if (S_ISDIR(sb.st_mode)) {
            if (virFileMakePath(devMountsPath[i]) < 0) {
                virReportSystemError(errno, _("Cannot create %s"),
                                     devMountsPath[i]);
                goto cleanup;
            }
        } else {
            if (virFileMakeParentPath(devMountsPath[i]) < 0 ||
                virFileTouch(devMountsPath[i], sb.st_mode) < 0) {
                virReportSystemError(errno, _("Cannot create %s"),
                                     devMountsPath[i]);
                goto cleanup;
            }
        }

        if (virFileMoveMount(devMountsSavePath[i], devMountsPath[i]) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    for (i = 0; i < ndevMountsPath; i++) {
        /* The path can be either a regular file or a dir. */
        if (virFileIsDir(devMountsSavePath[i]))
            rmdir(devMountsSavePath[i]);
        else
            unlink(devMountsSavePath[i]);
    }
    virStringListFreeCount(devMountsPath, ndevMountsPath);
    virStringListFreeCount(devMountsSavePath, ndevMountsPath);
    return ret;
}


int
qemuDomainCreateNamespace(virQEMUDriverPtr driver,
                          virDomainObjPtr vm)
{
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (virBitmapIsBitSet(cfg->namespaces, QEMU_DOMAIN_NS_MOUNT) &&
        qemuDomainEnableNamespace(vm, QEMU_DOMAIN_NS_MOUNT) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnref(cfg);
    return ret;
}


void
qemuDomainDestroyNamespace(virQEMUDriverPtr driver ATTRIBUTE_UNUSED,
                           virDomainObjPtr vm)
{
    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        qemuDomainDisableNamespace(vm, QEMU_DOMAIN_NS_MOUNT);
}


bool
qemuDomainNamespaceAvailable(qemuDomainNamespace ns ATTRIBUTE_UNUSED)
{
#if !defined(__linux__)
    /* Namespaces are Linux specific. */
    return false;

#else /* defined(__linux__) */

    switch (ns) {
    case QEMU_DOMAIN_NS_MOUNT:
# if !defined(HAVE_SYS_ACL_H) || !defined(WITH_SELINUX)
        /* We can't create the exact copy of paths if either of
         * these is not available. */
        return false;
# else
        if (virProcessNamespaceAvailable(VIR_PROCESS_NAMESPACE_MNT) < 0)
            return false;
# endif
        break;
    case QEMU_DOMAIN_NS_LAST:
        break;
    }

    return true;
#endif /* defined(__linux__) */
}


struct qemuDomainAttachDeviceMknodData {
    virQEMUDriverPtr driver;
    virDomainObjPtr vm;
    const char *file;
    const char *target;
    struct stat sb;
    void *acl;
#ifdef WITH_SELINUX
    char *tcon;
#endif
};


/* Our way of creating devices is highly linux specific */
#if defined(__linux__)
static int
qemuDomainAttachDeviceMknodHelper(pid_t pid ATTRIBUTE_UNUSED,
                                  void *opaque)
{
    struct qemuDomainAttachDeviceMknodData *data = opaque;
    int ret = -1;
    bool delDevice = false;
    bool isLink = S_ISLNK(data->sb.st_mode);
    bool isDev = S_ISCHR(data->sb.st_mode) || S_ISBLK(data->sb.st_mode);
    bool isReg = S_ISREG(data->sb.st_mode) || S_ISFIFO(data->sb.st_mode) || S_ISSOCK(data->sb.st_mode);
    bool isDir = S_ISDIR(data->sb.st_mode);

    qemuSecurityPostFork(data->driver->securityManager);

    if (virFileMakeParentPath(data->file) < 0) {
        virReportSystemError(errno,
                             _("Unable to create %s"), data->file);
        goto cleanup;
    }

    if (isLink) {
        VIR_DEBUG("Creating symlink %s -> %s", data->file, data->target);

        /* First, unlink the symlink target. Symlinks change and
         * therefore we have no guarantees that pre-existing
         * symlink is still valid. */
        if (unlink(data->file) < 0 &&
            errno != ENOENT) {
            virReportSystemError(errno,
                                 _("Unable to remove symlink %s"),
                                 data->file);
            goto cleanup;
        }

        if (symlink(data->target, data->file) < 0) {
            virReportSystemError(errno,
                                 _("Unable to create symlink %s (pointing to %s)"),
                                 data->file, data->target);
            goto cleanup;
        } else {
            delDevice = true;
        }
    } else if (isDev) {
        VIR_DEBUG("Creating dev %s (%d,%d)",
                  data->file, major(data->sb.st_rdev), minor(data->sb.st_rdev));
        if (mknod(data->file, data->sb.st_mode, data->sb.st_rdev) < 0) {
            /* Because we are not removing devices on hotunplug, or
             * we might be creating part of backing chain that
             * already exist due to a different disk plugged to
             * domain, accept EEXIST. */
            if (errno != EEXIST) {
                virReportSystemError(errno,
                                     _("Unable to create device %s"),
                                     data->file);
                goto cleanup;
            }
        } else {
            delDevice = true;
        }
    } else if (isReg || isDir) {
        /* We are not cleaning up disks on virDomainDetachDevice
         * because disk might be still in use by different disk
         * as its backing chain. This might however clash here.
         * Therefore do the cleanup here. */
        if (umount(data->file) < 0 &&
            errno != ENOENT && errno != EINVAL) {
            virReportSystemError(errno,
                                 _("Unable to umount %s"),
                                 data->file);
            goto cleanup;
        }
        if ((isReg && virFileTouch(data->file, data->sb.st_mode) < 0) ||
            (isDir && virFileMakePathWithMode(data->file, data->sb.st_mode) < 0))
            goto cleanup;
        delDevice = true;
        /* Just create the file here so that code below sets
         * proper owner and mode. Move the mount only after that. */
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("unsupported device type %s 0%o"),
                       data->file, data->sb.st_mode);
        goto cleanup;
    }

    if (lchown(data->file, data->sb.st_uid, data->sb.st_gid) < 0) {
        virReportSystemError(errno,
                             _("Failed to chown device %s"),
                             data->file);
        goto cleanup;
    }

    /* Symlinks don't have mode */
    if (!isLink &&
        chmod(data->file, data->sb.st_mode) < 0) {
        virReportSystemError(errno,
                             _("Failed to set permissions for device %s"),
                             data->file);
        goto cleanup;
    }

    /* Symlinks don't have ACLs. */
    if (!isLink &&
        virFileSetACLs(data->file, data->acl) < 0 &&
        errno != ENOTSUP) {
        virReportSystemError(errno,
                             _("Unable to set ACLs on %s"), data->file);
        goto cleanup;
    }

# ifdef WITH_SELINUX
    if (data->tcon &&
        lsetfilecon_raw(data->file, (VIR_SELINUX_CTX_CONST char *)data->tcon) < 0) {
        VIR_WARNINGS_NO_WLOGICALOP_EQUAL_EXPR
        if (errno != EOPNOTSUPP && errno != ENOTSUP) {
        VIR_WARNINGS_RESET
            virReportSystemError(errno,
                                 _("Unable to set SELinux label on %s"),
                                 data->file);
            goto cleanup;
        }
    }
# endif

    /* Finish mount process started earlier. */
    if ((isReg || isDir) &&
        virFileMoveMount(data->target, data->file) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    if (ret < 0 && delDevice) {
        if (isDir)
            virFileDeleteTree(data->file);
        else
            unlink(data->file);
    }
# ifdef WITH_SELINUX
    freecon(data->tcon);
# endif
    virFileFreeACLs(&data->acl);
    return ret;
}


static int
qemuDomainAttachDeviceMknodRecursive(virQEMUDriverPtr driver,
                                     virDomainObjPtr vm,
                                     const char *file,
                                     char * const *devMountsPath,
                                     size_t ndevMountsPath,
                                     unsigned int ttl)
{
    virQEMUDriverConfigPtr cfg = NULL;
    struct qemuDomainAttachDeviceMknodData data;
    int ret = -1;
    char *target = NULL;
    bool isLink;
    bool isReg;
    bool isDir;

    if (!ttl) {
        virReportSystemError(ELOOP,
                             _("Too many levels of symbolic links: %s"),
                             file);
        return ret;
    }

    memset(&data, 0, sizeof(data));

    data.driver = driver;
    data.vm = vm;
    data.file = file;

    if (lstat(file, &data.sb) < 0) {
        virReportSystemError(errno,
                             _("Unable to access %s"), file);
        return ret;
    }

    isLink = S_ISLNK(data.sb.st_mode);
    isReg = S_ISREG(data.sb.st_mode) || S_ISFIFO(data.sb.st_mode) || S_ISSOCK(data.sb.st_mode);
    isDir = S_ISDIR(data.sb.st_mode);

    if ((isReg || isDir) && STRPREFIX(file, DEVPREFIX)) {
        cfg = virQEMUDriverGetConfig(driver);
        if (!(target = qemuDomainGetPreservedMountPath(cfg, vm, file)))
            goto cleanup;

        if (virFileBindMountDevice(file, target) < 0)
            goto cleanup;

        data.target = target;
    } else if (isLink) {
        if (virFileReadLink(file, &target) < 0) {
            virReportSystemError(errno,
                                 _("unable to resolve symlink %s"),
                                 file);
            return ret;
        }

        if (IS_RELATIVE_FILE_NAME(target)) {
            char *c = NULL, *tmp = NULL, *fileTmp = NULL;

            if (VIR_STRDUP(fileTmp, file) < 0)
                goto cleanup;

            if ((c = strrchr(fileTmp, '/')))
                *(c + 1) = '\0';

            if (virAsprintf(&tmp, "%s%s", fileTmp, target) < 0) {
                VIR_FREE(fileTmp);
                goto cleanup;
            }
            VIR_FREE(fileTmp);
            VIR_FREE(target);
            target = tmp;
            tmp = NULL;
        }

        data.target = target;
    }

    /* Symlinks don't have ACLs. */
    if (!isLink &&
        virFileGetACLs(file, &data.acl) < 0 &&
        errno != ENOTSUP) {
        virReportSystemError(errno,
                             _("Unable to get ACLs on %s"), file);
        goto cleanup;
    }

# ifdef WITH_SELINUX
    if (lgetfilecon_raw(file, &data.tcon) < 0 &&
        (errno != ENOTSUP && errno != ENODATA)) {
        virReportSystemError(errno,
                             _("Unable to get SELinux label from %s"), file);
        goto cleanup;
    }
# endif

    if (STRPREFIX(file, DEVPREFIX)) {
        size_t i;

        for (i = 0; i < ndevMountsPath; i++) {
            if (STREQ(devMountsPath[i], "/dev"))
                continue;
            if (STRPREFIX(file, devMountsPath[i]))
                break;
        }

        if (i == ndevMountsPath) {
            if (qemuSecurityPreFork(driver->securityManager) < 0)
                goto cleanup;

            if (virProcessRunInMountNamespace(vm->pid,
                                              qemuDomainAttachDeviceMknodHelper,
                                              &data) < 0) {
                qemuSecurityPostFork(driver->securityManager);
                goto cleanup;
            }
            qemuSecurityPostFork(driver->securityManager);
        } else {
            VIR_DEBUG("Skipping dev %s because of %s mount point",
                      file, devMountsPath[i]);
        }
    }

    if (isLink &&
        qemuDomainAttachDeviceMknodRecursive(driver, vm, target,
                                             devMountsPath, ndevMountsPath,
                                             ttl -1) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
# ifdef WITH_SELINUX
    freecon(data.tcon);
# endif
    virFileFreeACLs(&data.acl);
    if (isReg && target)
        umount(target);
    VIR_FREE(target);
    virObjectUnref(cfg);
    return ret;
}


#else /* !defined(__linux__) */


static int
qemuDomainAttachDeviceMknodRecursive(virQEMUDriverPtr driver ATTRIBUTE_UNUSED,
                                     virDomainObjPtr vm ATTRIBUTE_UNUSED,
                                     const char *file ATTRIBUTE_UNUSED,
                                     char * const *devMountsPath ATTRIBUTE_UNUSED,
                                     size_t ndevMountsPath ATTRIBUTE_UNUSED,
                                     unsigned int ttl ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Namespaces are not supported on this platform."));
    return -1;
}


#endif /* !defined(__linux__) */


static int
qemuDomainAttachDeviceMknod(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            const char *file,
                            char * const *devMountsPath,
                            size_t ndevMountsPath)
{
    long symloop_max = sysconf(_SC_SYMLOOP_MAX);

    return qemuDomainAttachDeviceMknodRecursive(driver, vm, file,
                                                devMountsPath, ndevMountsPath,
                                                symloop_max);
}


static int
qemuDomainDetachDeviceUnlinkHelper(pid_t pid ATTRIBUTE_UNUSED,
                                   void *opaque)
{
    const char *path = opaque;

    VIR_DEBUG("Unlinking %s", path);
    if (unlink(path) < 0 && errno != ENOENT) {
        virReportSystemError(errno,
                             _("Unable to remove device %s"), path);
        return -1;
    }

    return 0;
}


static int
qemuDomainDetachDeviceUnlink(virQEMUDriverPtr driver ATTRIBUTE_UNUSED,
                             virDomainObjPtr vm,
                             const char *file,
                             char * const *devMountsPath,
                             size_t ndevMountsPath)
{
    int ret = -1;
    size_t i;

    if (STRPREFIX(file, DEVPREFIX)) {
        for (i = 0; i < ndevMountsPath; i++) {
            if (STREQ(devMountsPath[i], "/dev"))
                continue;
            if (STRPREFIX(file, devMountsPath[i]))
                break;
        }

        if (i == ndevMountsPath) {
            if (virProcessRunInMountNamespace(vm->pid,
                                              qemuDomainDetachDeviceUnlinkHelper,
                                              (void *)file) < 0)
                goto cleanup;
        }
    }

    ret = 0;
 cleanup:
    return ret;
}


static int
qemuDomainNamespaceMknodPaths(virDomainObjPtr vm,
                              const char **paths,
                              size_t npaths)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virQEMUDriverConfigPtr cfg;
    char **devMountsPath = NULL;
    size_t ndevMountsPath = 0;
    int ret = -1;
    size_t i;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        return 0;

    cfg = virQEMUDriverGetConfig(driver);
    if (qemuDomainGetPreservedMounts(cfg, vm,
                                     &devMountsPath, NULL,
                                     &ndevMountsPath) < 0)
        goto cleanup;

    for (i = 0; i < npaths; i++) {
        if (qemuDomainAttachDeviceMknod(driver,
                                        vm,
                                        paths[i],
                                        devMountsPath, ndevMountsPath) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    virStringListFreeCount(devMountsPath, ndevMountsPath);
    virObjectUnref(cfg);
    return ret;
}


static int
qemuDomainNamespaceMknodPath(virDomainObjPtr vm,
                             const char *path)
{
    const char *paths[] = { path };

    return qemuDomainNamespaceMknodPaths(vm, paths, 1);
}


static int
qemuDomainNamespaceUnlinkPaths(virDomainObjPtr vm,
                               const char **paths,
                               size_t npaths)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    virQEMUDriverConfigPtr cfg;
    char **devMountsPath = NULL;
    size_t ndevMountsPath = 0;
    size_t i;
    int ret = -1;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        return 0;

    cfg = virQEMUDriverGetConfig(driver);

    if (qemuDomainGetPreservedMounts(cfg, vm,
                                     &devMountsPath, NULL,
                                     &ndevMountsPath) < 0)
        goto cleanup;

    for (i = 0; i < npaths; i++) {
        if (qemuDomainDetachDeviceUnlink(driver, vm, paths[i],
                                         devMountsPath, ndevMountsPath) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    virStringListFreeCount(devMountsPath, ndevMountsPath);
    virObjectUnref(cfg);
    return ret;
}


static int
qemuDomainNamespaceUnlinkPath(virDomainObjPtr vm,
                              const char *path)
{
    const char *paths[] = { path };

    return qemuDomainNamespaceUnlinkPaths(vm, paths, 1);
}


int
qemuDomainNamespaceSetupDisk(virDomainObjPtr vm,
                             virStorageSourcePtr src)
{
    virStorageSourcePtr next;
    const char **paths = NULL;
    size_t npaths = 0;
    int ret = -1;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        return 0;

    for (next = src; virStorageSourceIsBacking(next); next = next->backingStore) {
        if (virStorageSourceIsEmpty(next) ||
            !virStorageSourceIsLocalStorage(next)) {
            /* Not creating device. Just continue. */
            continue;
        }

        if (VIR_APPEND_ELEMENT_COPY(paths, npaths, next->path) < 0)
            goto cleanup;
    }

    if (qemuDomainNamespaceMknodPaths(vm, paths, npaths) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(paths);
    return ret;
}


int
qemuDomainNamespaceTeardownDisk(virDomainObjPtr vm ATTRIBUTE_UNUSED,
                                virStorageSourcePtr src ATTRIBUTE_UNUSED)
{
    /* While in hotplug case we create the whole backing chain,
     * here we must limit ourselves. The disk we want to remove
     * might be a part of backing chain of another disk.
     * If you are reading these lines and have some spare time
     * you can come up with and algorithm that checks for that.
     * I don't, therefore: */
    return 0;
}


int
qemuDomainNamespaceSetupHostdev(virDomainObjPtr vm,
                                virDomainHostdevDefPtr hostdev)
{
    int ret = -1;
    char **paths = NULL;
    size_t i, npaths = 0;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        return 0;

    if (qemuDomainGetHostdevPath(NULL, hostdev, false, &npaths, &paths, NULL) < 0)
        goto cleanup;

    if (qemuDomainNamespaceMknodPaths(vm, (const char **)paths, npaths) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    for (i = 0; i < npaths; i++)
        VIR_FREE(paths[i]);
    VIR_FREE(paths);
    return ret;
}


int
qemuDomainNamespaceTeardownHostdev(virDomainObjPtr vm,
                                   virDomainHostdevDefPtr hostdev)
{
    int ret = -1;
    char **paths = NULL;
    size_t i, npaths = 0;

    if (!qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        return 0;

    if (qemuDomainGetHostdevPath(vm->def, hostdev, true,
                                 &npaths, &paths, NULL) < 0)
        goto cleanup;

    if (npaths != 0 &&
        qemuDomainNamespaceUnlinkPaths(vm, (const char **)paths, npaths) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    for (i = 0; i < npaths; i++)
        VIR_FREE(paths[i]);
    VIR_FREE(paths);
    return ret;
}


int
qemuDomainNamespaceSetupMemory(virDomainObjPtr vm,
                               virDomainMemoryDefPtr mem)
{
    if (mem->model != VIR_DOMAIN_MEMORY_MODEL_NVDIMM)
        return 0;

    if (qemuDomainNamespaceMknodPath(vm, mem->nvdimmPath) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceTeardownMemory(virDomainObjPtr vm,
                                  virDomainMemoryDefPtr mem)
{
    if (mem->model != VIR_DOMAIN_MEMORY_MODEL_NVDIMM)
        return 0;

    if (qemuDomainNamespaceUnlinkPath(vm, mem->nvdimmPath) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceSetupChardev(virDomainObjPtr vm,
                                virDomainChrDefPtr chr)
{
    const char *path;

    if (!(path = virDomainChrSourceDefGetPath(chr->source)))
        return 0;

    /* Socket created by qemu. It doesn't exist upfront. */
    if (chr->source->type == VIR_DOMAIN_CHR_TYPE_UNIX &&
        chr->source->data.nix.listen)
        return 0;

    if (qemuDomainNamespaceMknodPath(vm, path) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceTeardownChardev(virDomainObjPtr vm,
                                   virDomainChrDefPtr chr)
{
    const char *path = NULL;

    if (chr->source->type != VIR_DOMAIN_CHR_TYPE_DEV)
        return 0;

    path = chr->source->data.file.path;

    if (qemuDomainNamespaceUnlinkPath(vm, path) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceSetupRNG(virDomainObjPtr vm,
                            virDomainRNGDefPtr rng)
{
    const char *path = NULL;

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        path = rng->source.file;
        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    if (path && qemuDomainNamespaceMknodPath(vm, path) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceTeardownRNG(virDomainObjPtr vm,
                               virDomainRNGDefPtr rng)
{
    const char *path = NULL;

    switch ((virDomainRNGBackend) rng->backend) {
    case VIR_DOMAIN_RNG_BACKEND_RANDOM:
        path = rng->source.file;
        break;

    case VIR_DOMAIN_RNG_BACKEND_EGD:
    case VIR_DOMAIN_RNG_BACKEND_LAST:
        break;
    }

    if (path && qemuDomainNamespaceUnlinkPath(vm, path) < 0)
        return -1;

    return 0;
}


int
qemuDomainNamespaceSetupInput(virDomainObjPtr vm,
                              virDomainInputDefPtr input)
{
    const char *path = NULL;

    if (!(path = virDomainInputDefGetPath(input)))
        return 0;

    if (path && qemuDomainNamespaceMknodPath(vm, path) < 0)
        return -1;
    return 0;
}


int
qemuDomainNamespaceTeardownInput(virDomainObjPtr vm,
                                 virDomainInputDefPtr input)
{
    const char *path = NULL;

    if (!(path = virDomainInputDefGetPath(input)))
        return 0;

    if (path && qemuDomainNamespaceUnlinkPath(vm, path) < 0)
        return -1;

    return 0;
}


/**
 * qemuDomainDiskLookupByNodename:
 * @def: domain definition to look for the disk
 * @nodename: block backend node name to find
 * @src: filled with the specific backing store element if provided
 * @idx: index of @src in the backing chain, if provided
 *
 * Looks up the disk in the domain via @nodename and returns its definition.
 * Optionally fills @src and @idx if provided with the specific backing chain
 * element which corresponds to the node name.
 */
virDomainDiskDefPtr
qemuDomainDiskLookupByNodename(virDomainDefPtr def,
                               const char *nodename,
                               virStorageSourcePtr *src,
                               unsigned int *idx)
{
    size_t i;
    unsigned int srcindex;
    virStorageSourcePtr tmp = NULL;

    if (!idx)
        idx = &srcindex;

    if (src)
        *src = NULL;

    *idx = 0;

    for (i = 0; i < def->ndisks; i++) {
        if ((tmp = virStorageSourceFindByNodeName(def->disks[i]->src,
                                                  nodename, idx))) {
            if (src)
                *src = tmp;

            return def->disks[i];
        }
    }

    return NULL;
}


/**
 * qemuDomainDiskBackingStoreGetName:
 *
 * Creates a name using the indexed syntax (vda[1])for the given backing store
 * entry for a disk.
 */
char *
qemuDomainDiskBackingStoreGetName(virDomainDiskDefPtr disk,
                                  virStorageSourcePtr src ATTRIBUTE_UNUSED,
                                  unsigned int idx)
{
    char *ret = NULL;

    if (idx)
        ignore_value(virAsprintf(&ret, "%s[%d]", disk->dst, idx));
    else
        ignore_value(VIR_STRDUP(ret, disk->dst));

    return ret;
}


virStorageSourcePtr
qemuDomainGetStorageSourceByDevstr(const char *devstr,
                                   virDomainDefPtr def)
{
    virDomainDiskDefPtr disk = NULL;
    virStorageSourcePtr src = NULL;
    char *target = NULL;
    unsigned int idx;
    size_t i;

    if (virStorageFileParseBackingStoreStr(devstr, &target, &idx) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("failed to parse block device '%s'"), devstr);
        return NULL;
    }

    for (i = 0; i < def->ndisks; i++) {
        if (STREQ(target, def->disks[i]->dst)) {
            disk = def->disks[i];
            break;
        }
    }

    if (!disk) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("failed to find disk '%s'"), target);
        goto cleanup;
    }

    if (idx == 0)
        src = disk->src;
    else
        src = virStorageFileChainLookup(disk->src, NULL, NULL, idx, NULL);

 cleanup:
    VIR_FREE(target);
    return src;
}


static void
qemuDomainSaveCookieDispose(void *obj)
{
    qemuDomainSaveCookiePtr cookie = obj;

    VIR_DEBUG("cookie=%p", cookie);

    virCPUDefFree(cookie->cpu);
}


qemuDomainSaveCookiePtr
qemuDomainSaveCookieNew(virDomainObjPtr vm ATTRIBUTE_UNUSED)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuDomainSaveCookiePtr cookie = NULL;

    if (qemuDomainInitialize() < 0)
        goto error;

    if (!(cookie = virObjectNew(qemuDomainSaveCookieClass)))
        goto error;

    if (priv->origCPU && !(cookie->cpu = virCPUDefCopy(vm->def->cpu)))
        goto error;

    VIR_DEBUG("Save cookie %p, cpu=%p", cookie, cookie->cpu);

    return cookie;

 error:
    virObjectUnref(cookie);
    return NULL;
}


static int
qemuDomainSaveCookieParse(xmlXPathContextPtr ctxt ATTRIBUTE_UNUSED,
                          virObjectPtr *obj)
{
    qemuDomainSaveCookiePtr cookie = NULL;

    if (qemuDomainInitialize() < 0)
        goto error;

    if (!(cookie = virObjectNew(qemuDomainSaveCookieClass)))
        goto error;

    if (virCPUDefParseXML(ctxt, "./cpu[1]", VIR_CPU_TYPE_GUEST,
                          &cookie->cpu) < 0)
        goto error;

    *obj = (virObjectPtr) cookie;
    return 0;

 error:
    virObjectUnref(cookie);
    return -1;
}


static int
qemuDomainSaveCookieFormat(virBufferPtr buf,
                           virObjectPtr obj)
{
    qemuDomainSaveCookiePtr cookie = (qemuDomainSaveCookiePtr) obj;

    if (cookie->cpu &&
        virCPUDefFormatBufFull(buf, cookie->cpu, NULL) < 0)
        return -1;

    return 0;
}


virSaveCookieCallbacks virQEMUDriverDomainSaveCookie = {
    .parse = qemuDomainSaveCookieParse,
    .format = qemuDomainSaveCookieFormat,
};


/**
 * qemuDomainUpdateCPU:
 * @vm: domain which is being started
 * @cpu: CPU updated when the domain was running previously (before migration,
 *       snapshot, or save)
 * @origCPU: where to store the original CPU from vm->def in case @cpu was
 *           used instead
 *
 * Replace the CPU definition with the updated one when QEMU is new enough to
 * allow us to check extra features it is about to enable or disable when
 * starting a domain. The original CPU is stored in @origCPU.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuDomainUpdateCPU(virDomainObjPtr vm,
                    virCPUDefPtr cpu,
                    virCPUDefPtr *origCPU)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    *origCPU = NULL;

    if (!cpu || !vm->def->cpu ||
        !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_EXPANSION) ||
        virCPUDefIsEqual(vm->def->cpu, cpu, false))
        return 0;

    if (!(cpu = virCPUDefCopy(cpu)))
        return -1;

    VIR_DEBUG("Replacing CPU def with the updated one");

    *origCPU = vm->def->cpu;
    vm->def->cpu = cpu;

    return 0;
}


/**
 * qemuDomainFixupCPUS:
 * @vm: domain object
 * @origCPU: original CPU used when the domain was started
 *
 * Libvirt older than 3.9.0 could have messed up the expansion of host-model
 * CPU when reconnecting to a running domain by adding features QEMU does not
 * support (such as cmt). This API fixes both the actual CPU provided by QEMU
 * (stored in the domain object) and the @origCPU used when starting the
 * domain.
 *
 * This is safe even if the original CPU definition used mode='custom' (rather
 * than host-model) since we know QEMU was able to start the domain and thus
 * the CPU definitions do not contain any features unknown to QEMU.
 *
 * This function can only be used on an active domain or when restoring a
 * domain which was running.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuDomainFixupCPUs(virDomainObjPtr vm,
                    virCPUDefPtr *origCPU)
{
    virCPUDefPtr fixedCPU = NULL;
    virCPUDefPtr fixedOrig = NULL;
    virArch arch = vm->def->os.arch;
    int ret = -1;

    if (!ARCH_IS_X86(arch))
        return 0;

    if (!vm->def->cpu ||
        vm->def->cpu->mode != VIR_CPU_MODE_CUSTOM ||
        !vm->def->cpu->model)
        return 0;

    /* Missing origCPU means QEMU created exactly the same virtual CPU which
     * we asked for or libvirt was too old to mess up the translation from
     * host-model.
     */
    if (!*origCPU)
        return 0;

    if (virCPUDefFindFeature(vm->def->cpu, "cmt") &&
        (!(fixedCPU = virCPUDefCopyWithoutModel(vm->def->cpu)) ||
         virCPUDefCopyModelFilter(fixedCPU, vm->def->cpu, false,
                                  virQEMUCapsCPUFilterFeatures, &arch) < 0))
        goto cleanup;

    if (virCPUDefFindFeature(*origCPU, "cmt") &&
        (!(fixedOrig = virCPUDefCopyWithoutModel(*origCPU)) ||
         virCPUDefCopyModelFilter(fixedOrig, *origCPU, false,
                                  virQEMUCapsCPUFilterFeatures, &arch) < 0))
        goto cleanup;

    if (fixedCPU) {
        virCPUDefFree(vm->def->cpu);
        VIR_STEAL_PTR(vm->def->cpu, fixedCPU);
    }

    if (fixedOrig) {
        virCPUDefFree(*origCPU);
        VIR_STEAL_PTR(*origCPU, fixedOrig);
    }

    ret = 0;

 cleanup:
    virCPUDefFree(fixedCPU);
    virCPUDefFree(fixedOrig);
    return ret;
}


char *
qemuDomainGetMachineName(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    char *ret = NULL;

    if (vm->pid > 0) {
        ret = virSystemdGetMachineNameByPID(vm->pid);
        if (!ret)
            virResetLastError();
    }

    if (!ret)
        ret = virDomainGenerateMachineName("qemu", vm->def->id, vm->def->name,
                                           virQEMUDriverIsPrivileged(driver));

    return ret;
}


/* Check whether the device address is using either 'ccw' or default s390
 * address format and whether that's "legal" for the current qemu and/or
 * guest os.machine type. This is the corollary to the code which doesn't
 * find the address type set using an emulator that supports either 'ccw'
 * or s390 and sets the address type based on the capabilities.
 *
 * If the address is using 'ccw' or s390 and it's not supported, generate
 * an error and return false; otherwise, return true.
 */
bool
qemuDomainCheckCCWS390AddressSupport(const virDomainDef *def,
                                     virDomainDeviceInfo info,
                                     virQEMUCapsPtr qemuCaps,
                                     const char *devicename)
{
    if (info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW) {
        if (!qemuDomainIsS390CCW(def)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("cannot use CCW address type for device "
                             "'%s' using machine type '%s'"),
                       devicename, def->os.machine);
            return false;
        } else if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_CCW)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("CCW address type is not supported by "
                             "this QEMU"));
            return false;
        }
    } else if (info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390) {
        if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_VIRTIO_S390)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("virtio S390 address type is not supported by "
                             "this QEMU"));
            return false;
        }
    }
    return true;
}


/**
 * qemuDomainPrepareDiskSourceChain:
 *
 * @disk: Disk config object
 * @src: source to start from
 * @cfg: qemu driver config object
 *
 * Prepares various aspects of the disk source and it's backing chain. This
 * function should be also called for detected backing chains. If @src is NULL
 * the root source is used.
 */
int
qemuDomainPrepareDiskSourceChain(virDomainDiskDefPtr disk,
                                 virStorageSourcePtr src,
                                 virQEMUDriverConfigPtr cfg,
                                 virQEMUCapsPtr qemuCaps)
{
    virStorageSourcePtr n;

    if (!src)
        src = disk->src;

    /* transfer properties valid only for the top level image */
    src->detect_zeroes = disk->detect_zeroes;

    for (n = src; virStorageSourceIsBacking(n); n = n->backingStore) {
        if (n->type == VIR_STORAGE_TYPE_NETWORK &&
            n->protocol == VIR_STORAGE_NET_PROTOCOL_GLUSTER &&
            virQEMUCapsGet(qemuCaps, QEMU_CAPS_GLUSTER_DEBUG_LEVEL)) {
            n->debug = true;
            n->debugLevel = cfg->glusterDebugLevel;
        }

        if (qemuDomainValidateStorageSource(n, qemuCaps) < 0)
            return -1;

        /* transfer properties valid for the full chain */
        n->iomode = disk->iomode;
        n->cachemode = disk->cachemode;
        n->discard = disk->discard;

        if (disk->device == VIR_DOMAIN_DISK_DEVICE_FLOPPY)
            n->floppyimg = true;
    }

    return 0;
}


static void
qemuDomainPrepareDiskCachemode(virDomainDiskDefPtr disk)
{
    if (disk->cachemode == VIR_DOMAIN_DISK_CACHE_DEFAULT &&
        disk->src->shared && !disk->src->readonly)
        disk->cachemode = VIR_DOMAIN_DISK_CACHE_DISABLE;
}


int
qemuDomainPrepareDiskSource(virDomainDiskDefPtr disk,
                            qemuDomainObjPrivatePtr priv,
                            virQEMUDriverConfigPtr cfg)
{
    qemuDomainPrepareDiskCachemode(disk);

    if (qemuDomainPrepareDiskSourceTLS(disk->src, cfg) < 0)
        return -1;

    if (qemuDomainSecretDiskPrepare(priv, disk) < 0)
        return -1;

    if (qemuDomainPrepareDiskSourceChain(disk, NULL, cfg, priv->qemuCaps) < 0)
        return -1;

    return 0;
}


/**
 * qemuDomainDiskCachemodeFlags:
 *
 * Converts disk cachemode to the cache mode options for qemu. Returns -1 for
 * invalid @cachemode values and fills the flags and returns 0 on success.
 * Flags may be NULL.
 */
int
qemuDomainDiskCachemodeFlags(int cachemode,
                             bool *writeback,
                             bool *direct,
                             bool *noflush)
{
    bool dummy;

    if (!writeback)
        writeback = &dummy;

    if (!direct)
        direct = &dummy;

    if (!noflush)
        noflush = &dummy;

    /* Mapping of cache modes to the attributes according to qemu-options.hx
     *              │ cache.writeback   cache.direct   cache.no-flush
     * ─────────────┼─────────────────────────────────────────────────
     * writeback    │ true              false          false
     * none         │ true              true           false
     * writethrough │ false             false          false
     * directsync   │ false             true           false
     * unsafe       │ true              false          true
     */
    switch ((virDomainDiskCache) cachemode) {
    case VIR_DOMAIN_DISK_CACHE_DISABLE: /* 'none' */
        *writeback = true;
        *direct = true;
        *noflush = false;
        break;

    case VIR_DOMAIN_DISK_CACHE_WRITETHRU:
        *writeback = false;
        *direct = false;
        *noflush = false;
        break;

    case VIR_DOMAIN_DISK_CACHE_WRITEBACK:
        *writeback = true;
        *direct = false;
        *noflush = false;
        break;

    case VIR_DOMAIN_DISK_CACHE_DIRECTSYNC:
        *writeback = false;
        *direct = true;
        *noflush = false;
        break;

    case VIR_DOMAIN_DISK_CACHE_UNSAFE:
        *writeback = true;
        *direct = false;
        *noflush = true;
        break;

    case VIR_DOMAIN_DISK_CACHE_DEFAULT:
    case VIR_DOMAIN_DISK_CACHE_LAST:
    default:
        virReportEnumRangeError(virDomainDiskCache, cachemode);
        return -1;
    }

    return 0;
}


void
qemuProcessEventFree(struct qemuProcessEvent *event)
{
    if (!event)
        return;

    switch (event->eventType) {
    case QEMU_PROCESS_EVENT_GUESTPANIC:
        qemuMonitorEventPanicInfoFree(event->data);
        break;
    case QEMU_PROCESS_EVENT_WATCHDOG:
    case QEMU_PROCESS_EVENT_DEVICE_DELETED:
    case QEMU_PROCESS_EVENT_NIC_RX_FILTER_CHANGED:
    case QEMU_PROCESS_EVENT_SERIAL_CHANGED:
    case QEMU_PROCESS_EVENT_BLOCK_JOB:
    case QEMU_PROCESS_EVENT_MONITOR_EOF:
        VIR_FREE(event->data);
        break;
    case QEMU_PROCESS_EVENT_LAST:
        break;
    }
    VIR_FREE(event);
}
