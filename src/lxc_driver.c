/*
 * Copyright IBM Corp. 2008
 *
 * lxc_driver.c: linux container driver functions
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <config.h>

#ifdef WITH_LXC

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/utsname.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <wait.h>

#include "lxc_conf.h"
#include "lxc_container.h"
#include "lxc_driver.h"
#include "driver.h"
#include "internal.h"
#include "util.h"

/* debug macros */
#define DEBUG(fmt,...) VIR_DEBUG(__FILE__, fmt, __VA_ARGS__)
#define DEBUG0(msg) VIR_DEBUG(__FILE__, "%s", msg)

/*
 * GLibc headers are behind the kernel, so we define these
 * constants if they're not present already.
 */

#ifndef CLONE_NEWPID
#define CLONE_NEWPID  0x20000000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS  0x04000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC  0x08000000
#endif

static int lxcStartup(void);
static int lxcShutdown(void);
static lxc_driver_t *lxc_driver = NULL;

/* Functions */
static int lxcDummyChild( void *argv ATTRIBUTE_UNUSED )
{
    exit(0);
}

static int lxcCheckContainerSupport( void )
{
    int rc = 0;
    int flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWUSER|
        CLONE_NEWIPC|SIGCHLD;
    int cpid;
    char *childStack;
    char *stack;
    int childStatus;

    stack = malloc(getpagesize() * 4);
    if(!stack) {
        DEBUG0("Unable to allocate stack");
        rc = -1;
        goto check_complete;
    }

    childStack = stack + (getpagesize() * 4);

    cpid = clone(lxcDummyChild, childStack, flags, NULL);
    if ((0 > cpid) && (EINVAL == errno)) {
        DEBUG0("clone call returned EINVAL, container support is not enabled");
        rc = -1;
    } else {
        waitpid(cpid, &childStatus, 0);
    }

    free(stack);

check_complete:
    return rc;
}

static const char *lxcProbe(void)
{
#ifdef __linux__
    if (0 == lxcCheckContainerSupport()) {
        return("lxc:///");
    }
#endif
    return(NULL);
}

static virDrvOpenStatus lxcOpen(virConnectPtr conn,
                                xmlURIPtr uri,
                                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                int flags ATTRIBUTE_UNUSED)
{
    uid_t uid = getuid();

    /* Check that the user is root */
    if (0 != uid) {
        goto declineConnection;
    }

    if (lxc_driver == NULL)
        goto declineConnection;

    /* Verify uri was specified */
    if ((NULL == uri) || (NULL == uri->scheme)) {
        goto declineConnection;
    }

    /* Check that the uri scheme is lxc */
    if (STRNEQ(uri->scheme, "lxc")) {
        goto declineConnection;
    }

    conn->privateData = lxc_driver;

    return VIR_DRV_OPEN_SUCCESS;

declineConnection:
    return VIR_DRV_OPEN_DECLINED;
}

static int lxcClose(virConnectPtr conn)
{
    conn->privateData = NULL;
    return 0;
}

static virDomainPtr lxcDomainLookupByID(virConnectPtr conn,
                                        int id)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm = lxcFindVMByID(driver, id);
    virDomainPtr dom;

    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        return NULL;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) {
        dom->id = vm->def->id;
    }

    return dom;
}

static virDomainPtr lxcDomainLookupByUUID(virConnectPtr conn,
                                          const unsigned char *uuid)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm = lxcFindVMByUUID(driver, uuid);
    virDomainPtr dom;

    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        return NULL;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) {
        dom->id = vm->def->id;
    }

    return dom;
}

static virDomainPtr lxcDomainLookupByName(virConnectPtr conn,
                                          const char *name)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm = lxcFindVMByName(driver, name);
    virDomainPtr dom;

    if (!vm) {
        lxcError(conn, NULL, VIR_ERR_NO_DOMAIN, NULL);
        return NULL;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) {
        dom->id = vm->def->id;
    }

    return dom;
}

static int lxcListDomains(virConnectPtr conn, int *ids, int nids)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm;
    int numDoms = 0;

    for (vm = driver->vms; vm && (numDoms < nids); vm = vm->next) {
        if (lxcIsActiveVM(vm)) {
            ids[numDoms] = vm->def->id;
            numDoms++;
        }
    }

    return numDoms;
}

static int lxcNumDomains(virConnectPtr conn)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    return driver->nactivevms;
}

static int lxcListDefinedDomains(virConnectPtr conn,
                                 char **const names, int nnames)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm;
    int numDoms = 0;
    int i;

    for (vm = driver->vms; vm && (numDoms < nnames); vm = vm->next) {
        if (!lxcIsActiveVM(vm)) {
            if (!(names[numDoms] = strdup(vm->def->name))) {
                lxcError(conn, NULL, VIR_ERR_NO_MEMORY, "names");
                goto cleanup;
            }

            numDoms++;
        }

    }

    return numDoms;

 cleanup:
    for (i = 0 ; i < numDoms ; i++) {
        free(names[i]);
    }

    return -1;
}


static int lxcNumDefinedDomains(virConnectPtr conn)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    return driver->ninactivevms;
}

static virDomainPtr lxcDomainDefine(virConnectPtr conn, const char *xml)
{
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_def_t *def;
    lxc_vm_t *vm;
    virDomainPtr dom;

    if (!(def = lxcParseVMDef(conn, xml, NULL))) {
        return NULL;
    }

    if (!(vm = lxcAssignVMDef(conn, driver, def))) {
        lxcFreeVMDef(def);
        return NULL;
    }

    if (lxcSaveVMDef(conn, driver, vm, def) < 0) {
        lxcRemoveInactiveVM(driver, vm);
        return NULL;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) {
        dom->id = vm->def->id;
    }

    return dom;
}

static int lxcDomainUndefine(virDomainPtr dom)
{
    lxc_driver_t *driver = (lxc_driver_t *)dom->conn->privateData;
    lxc_vm_t *vm = lxcFindVMByUUID(driver, dom->uuid);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with matching uuid"));
        return -1;
    }

    if (lxcIsActiveVM(vm)) {
        lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                 _("cannot delete active domain"));
        return -1;
    }

    if (lxcDeleteConfig(dom->conn, driver, vm->configFile, vm->def->name) < 0) {
        return -1;
    }

    vm->configFile[0] = '\0';

    lxcRemoveInactiveVM(driver, vm);

    return 0;
}

static int lxcDomainGetInfo(virDomainPtr dom,
                            virDomainInfoPtr info)
{
    lxc_driver_t *driver = (lxc_driver_t *)dom->conn->privateData;
    lxc_vm_t *vm = lxcFindVMByUUID(driver, dom->uuid);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with matching uuid"));
        return -1;
    }

    info->state = vm->state;

    if (!lxcIsActiveVM(vm)) {
        info->cpuTime = 0;
    } else {
        info->cpuTime = 0;
    }

    info->maxMem = vm->def->maxMemory;
    info->memory = vm->def->maxMemory;
    info->nrVirtCpu = 1;

    return 0;
}

static char *lxcGetOSType(virDomainPtr dom ATTRIBUTE_UNUSED)
{
    /* Linux containers only run on Linux */
    return strdup("linux");
}

static char *lxcDomainDumpXML(virDomainPtr dom,
                              int flags ATTRIBUTE_UNUSED)
{
    lxc_driver_t *driver = (lxc_driver_t *)dom->conn->privateData;
    lxc_vm_t *vm = lxcFindVMByUUID(driver, dom->uuid);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with matching uuid"));
        return NULL;
    }

    return lxcGenerateXML(dom->conn, driver, vm, vm->def);
}

/**
 * lxcStartContainer:
 * @conn: pointer to connection
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure
 *
 * Starts a container process by calling clone() with the namespace flags
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcStartContainer(virConnectPtr conn,
                             lxc_driver_t* driver,
                             lxc_vm_t *vm)
{
    int rc = -1;
    int flags;
    int stacksize = getpagesize() * 4;
    void *stack, *stacktop;

    /* allocate a stack for the container */
    stack = malloc(stacksize);
    if (!stack) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                 _("unable to allocate container stack"));
        goto error_exit;
    }
    stacktop = (char*)stack + stacksize;

    flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWUSER|CLONE_NEWIPC|SIGCHLD;

    vm->def->id = clone(lxcChild, stacktop, flags, (void *)vm);

    DEBUG("clone() returned, %d", vm->def->id);

    if (vm->def->id < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("clone() failed, %s"), strerror(errno));
        goto error_exit;
    }

    lxcSaveConfig(NULL, driver, vm, vm->def);

    rc = 0;

error_exit:
    return rc;
}

/**
 * lxcPutTtyInRawMode:
 * @conn: pointer to connection
 * @ttyDev: file descriptor for tty
 *
 * Sets tty attributes via cfmakeraw()
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcPutTtyInRawMode(virConnectPtr conn, int ttyDev)
{
    int rc = -1;

    struct termios ttyAttr;

    if (tcgetattr(ttyDev, &ttyAttr) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 "tcgetattr() failed: %s", strerror(errno));
        goto cleanup;
    }

    cfmakeraw(&ttyAttr);

    if (tcsetattr(ttyDev, TCSADRAIN, &ttyAttr) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 "tcsetattr failed: %s", strerror(errno));
        goto cleanup;
    }

    rc = 0;

cleanup:
    return rc;
}

/**
 * lxcSetupTtyTunnel:
 * @conn: pointer to connection
 * @vmDef: pointer to virtual machine definition structure
 * @ttyDev: pointer to int.  On success will be set to fd for master
 * end of tty
 *
 * Opens and configures the parent side tty
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcSetupTtyTunnel(virConnectPtr conn,
                             lxc_vm_def_t *vmDef,
                             int* ttyDev)
{
    int rc = -1;
    char *ptsStr;

    if (0 < strlen(vmDef->tty)) {
        *ttyDev = open(vmDef->tty, O_RDWR|O_NOCTTY|O_NONBLOCK);
        if (*ttyDev < 0) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "open() tty failed: %s", strerror(errno));
            goto setup_complete;
        }

        rc = grantpt(*ttyDev);
        if (rc < 0) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "grantpt() failed: %s", strerror(errno));
            goto setup_complete;
        }

        rc = unlockpt(*ttyDev);
        if (rc < 0) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "unlockpt() failed: %s", strerror(errno));
            goto setup_complete;
        }

        /* get the name and print it to stdout */
        ptsStr = ptsname(*ttyDev);
        if (ptsStr == NULL) {
            lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                     "ptsname() failed");
            goto setup_complete;
        }
        /* This value needs to be stored in the container configuration file */
        if (STRNEQ(ptsStr, vmDef->tty)) {
            strcpy(vmDef->tty, ptsStr);
        }

        /* Enter raw mode, so all characters are passed directly to child */
        if (lxcPutTtyInRawMode(conn, *ttyDev) < 0) {
            goto setup_complete;
        }

    } else {
        *ttyDev = -1;
    }

    rc = 0;

setup_complete:
    if((0 != rc) && (*ttyDev > 0)) {
        close(*ttyDev);
    }

    return rc;
}

/**
 * lxcSetupContainerTty:
 * @conn: pointer to connection
 * @ttymaster: pointer to int.  On success, set to fd for master end
 * @ttyName: On success, will point to string slave end of tty.  Caller
 * must free when done (such as in lxcFreeVM).
 *
 * Opens and configures container tty.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcSetupContainerTty(virConnectPtr conn,
                                int *ttymaster,
                                char **ttyName)
{
    int rc = -1;
    char tempTtyName[PATH_MAX];

    *ttymaster = posix_openpt(O_RDWR|O_NOCTTY);
    if (*ttymaster < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("posix_openpt failed: %s"), strerror(errno));
        goto cleanup;
    }

    if (unlockpt(*ttymaster) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("unlockpt failed: %s"), strerror(errno));
        goto cleanup;
    }

    if (0 != ptsname_r(*ttymaster, tempTtyName, sizeof(tempTtyName))) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("ptsname_r failed: %s"), strerror(errno));
        goto cleanup;
    }

    *ttyName = malloc(sizeof(char) * (strlen(tempTtyName) + 1));
    if (NULL == ttyName) {
        lxcError(conn, NULL, VIR_ERR_NO_MEMORY,
                 _("unable to allocate container name string"));
        goto cleanup;
    }

    strcpy(*ttyName, tempTtyName);

    rc = 0;

cleanup:
    if (0 != rc) {
        if (-1 != *ttymaster) {
            close(*ttymaster);
        }
    }

    return rc;
}

/**
 * lxcTtyForward:
 * @fd1: Open fd
 * @fd1: Open fd
 *
 * Forwards traffic between fds.  Data read from fd1 will be written to fd2
 * Data read from fd2 will be written to fd1.  This process loops forever.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcTtyForward(int fd1, int fd2)
{
    int rc = -1;
    int i;
    char buf[2];
    struct pollfd fds[2];
    int numFds = 0;

    if (0 <= fd1) {
        fds[numFds].fd = fd1;
        fds[numFds].events = POLLIN;
        ++numFds;
    }

    if (0 <= fd2) {
        fds[numFds].fd = fd2;
        fds[numFds].events = POLLIN;
        ++numFds;
    }

    if (0 == numFds) {
        DEBUG0("No fds to monitor, return");
        goto cleanup;
    }

    while (1) {
        if ((rc = poll(fds, numFds, -1)) <= 0) {

            if ((0 == rc) || (errno == EINTR) || (errno == EAGAIN)) {
                continue;
            }

            lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("poll returned error: %s"), strerror(errno));
            goto cleanup;
        }

        for (i = 0; i < numFds; ++i) {
            if (!fds[i].revents) {
                continue;
            }

            if (fds[i].revents & POLLIN) {
                if (1 != (saferead(fds[i].fd, buf, 1))) {
                    lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             _("read of fd %d failed: %s"), i,
                             strerror(errno));
                    goto cleanup;
                }

                if (1 < numFds) {
                    if (1 != (safewrite(fds[i ^ 1].fd, buf, 1))) {
                        lxcError(NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 _("write to fd %d failed: %s"), i,
                                 strerror(errno));
                        goto cleanup;
                    }

                }

            }

        }

    }

    rc = 0;

cleanup:
    return rc;
}

/**
 * lxcVmStart:
 * @conn: pointer to connection
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure
 *
 * Starts a vm
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcVmStart(virConnectPtr conn,
                      lxc_driver_t * driver,
                      lxc_vm_t * vm)
{
    int rc = -1;
    lxc_vm_def_t *vmDef = vm->def;

    /* open parent tty */
    if (lxcSetupTtyTunnel(conn, vmDef, &vm->parentTty) < 0) {
        goto cleanup;
    }

    /* open container tty */
    if (lxcSetupContainerTty(conn, &(vm->containerTtyFd), &(vm->containerTty)) < 0) {
        goto cleanup;
    }

    /* fork process to handle the tty io forwarding */
    if ((vm->pid = fork()) < 0) {
        lxcError(conn, NULL, VIR_ERR_INTERNAL_ERROR,
                 _("unable to fork tty forwarding process: %s"),
                 strerror(errno));
        goto cleanup;
    }

    if (vm->pid  == 0) {
        /* child process calls forward routine */
        lxcTtyForward(vm->parentTty, vm->containerTtyFd);
    }

    close(vm->parentTty);
    close(vm->containerTtyFd);

    rc = lxcStartContainer(conn, driver, vm);

    if (rc == 0) {
        vm->state = VIR_DOMAIN_RUNNING;
        driver->ninactivevms--;
        driver->nactivevms++;
    }

cleanup:
    return rc;
}

/**
 * lxcDomainStart:
 * @dom: domain to start
 *
 * Looks up domain and starts it.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainStart(virDomainPtr dom)
{
    int rc = -1;
    virConnectPtr conn = dom->conn;
    lxc_driver_t *driver = (lxc_driver_t *)(conn->privateData);
    lxc_vm_t *vm = lxcFindVMByName(driver, dom->name);

    if (!vm) {
        lxcError(conn, dom, VIR_ERR_INVALID_DOMAIN,
                 "no domain with uuid");
        goto cleanup;
    }

    rc = lxcVmStart(conn, driver, vm);

cleanup:
    return rc;
}

/**
 * lxcDomainCreateAndStart:
 * @conn: pointer to connection
 * @xml: XML definition of domain
 * @flags: Unused
 *
 * Creates a domain based on xml and starts it
 *
 * Returns 0 on success or -1 in case of error
 */
static virDomainPtr
lxcDomainCreateAndStart(virConnectPtr conn,
                        const char *xml,
                        unsigned int flags ATTRIBUTE_UNUSED) {
    lxc_driver_t *driver = (lxc_driver_t *)conn->privateData;
    lxc_vm_t *vm;
    lxc_vm_def_t *def;
    virDomainPtr dom = NULL;

    if (!(def = lxcParseVMDef(conn, xml, NULL))) {
        goto return_point;
    }

    if (!(vm = lxcAssignVMDef(conn, driver, def))) {
        lxcFreeVMDef(def);
        goto return_point;
    }

    if (lxcSaveVMDef(conn, driver, vm, def) < 0) {
        lxcRemoveInactiveVM(driver, vm);
        return NULL;
    }

    if (lxcVmStart(conn, driver, vm) < 0) {
        lxcRemoveInactiveVM(driver, vm);
        goto return_point;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) {
        dom->id = vm->def->id;
    }

return_point:
    return dom;
}

/**
 * lxcDomainShutdown:
 * @dom: Ptr to domain to shutdown
 *
 * Sends SIGINT to container root process to request it to shutdown
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainShutdown(virDomainPtr dom)
{
    int rc = -1;
    lxc_driver_t *driver = (lxc_driver_t*)dom->conn->privateData;
    lxc_vm_t *vm = lxcFindVMByID(driver, dom->id);

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with id %d"), dom->id);
        goto error_out;
    }

    if (0 > (kill(vm->def->id, SIGINT))) {
        if (ESRCH != errno) {
            lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                     _("sending SIGTERM failed: %s"), strerror(errno));

            goto error_out;
        }
    }

    vm->state = VIR_DOMAIN_SHUTDOWN;

    rc = 0;

error_out:
    return rc;
}

/**
 * lxcDomainDestroy:
 * @dom: Ptr to domain to destroy
 *
 * Sends SIGKILL to container root process to terminate the container
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainDestroy(virDomainPtr dom)
{
    int rc = -1;
    int waitRc;
    lxc_driver_t *driver = (lxc_driver_t*)dom->conn->privateData;
    lxc_vm_t *vm = lxcFindVMByID(driver, dom->id);
    int childStatus;

    if (!vm) {
        lxcError(dom->conn, dom, VIR_ERR_INVALID_DOMAIN,
                 _("no domain with id %d"), dom->id);
        goto error_out;
    }

    if (0 > (kill(vm->def->id, SIGKILL))) {
        if (ESRCH != errno) {
            lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                     _("sending SIGKILL failed: %s"), strerror(errno));

            goto error_out;
        }
    }

    vm->state = VIR_DOMAIN_SHUTDOWN;

    while (((waitRc = waitpid(vm->def->id, &childStatus, 0)) == -1) &&
           errno == EINTR);

    if (waitRc != vm->def->id) {
        lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                 _("waitpid failed to wait for container %d: %d %s"),
                 vm->def->id, waitRc, strerror(errno));
        goto kill_tty;
    }

    rc = WEXITSTATUS(childStatus);
    DEBUG("container exited with rc: %d", rc);

kill_tty:
    if (0 > (kill(vm->pid, SIGKILL))) {
        if (ESRCH != errno) {
            lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                     _("sending SIGKILL to tty process failed: %s"),
                     strerror(errno));

            goto tty_error_out;
        }
    }

    while (((waitRc = waitpid(vm->pid, &childStatus, 0)) == -1) &&
           errno == EINTR);

    if (waitRc != vm->pid) {
        lxcError(dom->conn, dom, VIR_ERR_INTERNAL_ERROR,
                 _("waitpid failed to wait for tty %d: %d %s"),
                 vm->pid, waitRc, strerror(errno));
    }

tty_error_out:
    vm->state = VIR_DOMAIN_SHUTOFF;
    vm->pid = -1;
    vm->def->id = -1;
    driver->nactivevms--;
    driver->ninactivevms++;

    rc = 0;

error_out:
    return rc;
}

static int lxcStartup(void)
{
    uid_t uid = getuid();

    /* Check that the user is root */
    if (0 != uid) {
        return -1;
    }

    lxc_driver = calloc(1, sizeof(lxc_driver_t));
    if (NULL == lxc_driver) {
        return -1;
    }

    /* Check that this is a container enabled kernel */
    if(0 != lxcCheckContainerSupport()) {
        return -1;
    }


    /* Call function to load lxc driver configuration information */
    if (lxcLoadDriverConfig(lxc_driver) < 0) {
        lxcShutdown();
        return -1;
    }

    /* Call function to load the container configuration files */
    if (lxcLoadContainerInfo(lxc_driver) < 0) {
        lxcShutdown();
        return -1;
    }

    return 0;
}

static void lxcFreeDriver(lxc_driver_t *driver)
{
    free(driver->configDir);
    free(driver);
}

static int lxcShutdown(void)
{
    if (lxc_driver == NULL)
        return(-1);
    lxcFreeVMs(lxc_driver->vms);
    lxc_driver->vms = NULL;
    lxcFreeDriver(lxc_driver);
    lxc_driver = NULL;

    return 0;
}

/**
 * lxcActive:
 *
 * Checks if the LXC daemon is active, i.e. has an active domain
 *
 * Returns 1 if active, 0 otherwise
 */
static int
lxcActive(void) {
    if (lxc_driver == NULL)
        return(0);
    /* If we've any active networks or guests, then we
     * mark this driver as active
     */
    if (lxc_driver->nactivevms)
        return 1;

    /* Otherwise we're happy to deal with a shutdown */
    return 0;
}


/* Function Tables */
static virDriver lxcDriver = {
    VIR_DRV_LXC, /* the number virDrvNo */
    "LXC", /* the name of the driver */
    LIBVIR_VERSION_NUMBER, /* the version of the backend */
    lxcProbe, /* probe */
    lxcOpen, /* open */
    lxcClose, /* close */
    NULL, /* supports_feature */
    NULL, /* type */
    NULL, /* version */
    NULL, /* getHostname */
    NULL, /* getURI */
    NULL, /* getMaxVcpus */
    NULL, /* nodeGetInfo */
    NULL, /* getCapabilities */
    lxcListDomains, /* listDomains */
    lxcNumDomains, /* numOfDomains */
    lxcDomainCreateAndStart, /* domainCreateLinux */
    lxcDomainLookupByID, /* domainLookupByID */
    lxcDomainLookupByUUID, /* domainLookupByUUID */
    lxcDomainLookupByName, /* domainLookupByName */
    NULL, /* domainSuspend */
    NULL, /* domainResume */
    lxcDomainShutdown, /* domainShutdown */
    NULL, /* domainReboot */
    lxcDomainDestroy, /* domainDestroy */
    lxcGetOSType, /* domainGetOSType */
    NULL, /* domainGetMaxMemory */
    NULL, /* domainSetMaxMemory */
    NULL, /* domainSetMemory */
    lxcDomainGetInfo, /* domainGetInfo */
    NULL, /* domainSave */
    NULL, /* domainRestore */
    NULL, /* domainCoreDump */
    NULL, /* domainSetVcpus */
    NULL, /* domainPinVcpu */
    NULL, /* domainGetVcpus */
    NULL, /* domainGetMaxVcpus */
    lxcDomainDumpXML, /* domainDumpXML */
    lxcListDefinedDomains, /* listDefinedDomains */
    lxcNumDefinedDomains, /* numOfDefinedDomains */
    lxcDomainStart, /* domainCreate */
    lxcDomainDefine, /* domainDefineXML */
    lxcDomainUndefine, /* domainUndefine */
    NULL, /* domainAttachDevice */
    NULL, /* domainDetachDevice */
    NULL, /* domainGetAutostart */
    NULL, /* domainSetAutostart */
    NULL, /* domainGetSchedulerType */
    NULL, /* domainGetSchedulerParameters */
    NULL, /* domainSetSchedulerParameters */
    NULL, /* domainMigratePrepare */
    NULL, /* domainMigratePerform */
    NULL, /* domainMigrateFinish */
    NULL, /* domainBlockStats */
    NULL, /* domainInterfaceStats */
    NULL, /* nodeGetCellsFreeMemory */
    NULL, /* getFreeMemory */
};


static virStateDriver lxcStateDriver = {
    lxcStartup,
    lxcShutdown,
    NULL, /* reload */
    lxcActive,
};

int lxcRegister(void)
{
    virRegisterDriver(&lxcDriver);
    virRegisterStateDriver(&lxcStateDriver);
    return 0;
}

#endif /* WITH_LXC */

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
