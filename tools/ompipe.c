/* ompipe.c
 * This is the implementation of the build-in pipe output module.
 * Note that this module stems back to the "old" (4.4.2 and below)
 * omfile. There were some issues with the new omfile code and pipes
 * (namely in regard to xconsole), so we took out the pipe code and moved
 * that to a separate module. That a) immediately solves the issue for a
 * less common use case and probably makes it much easier to enhance
 * file and pipe support (now independently) in the future (we always
 * needed to think about pipes in omfile so far, what we now no longer
 * need to, hopefully resulting in reduction of complexity).
 *
 * NOTE: read comments in module-template.h to understand how this pipe
 *       works!
 *
 * Copyright 2007-2012 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#include "syslogd.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "ompipe.h"
#include "omfile.h" /* for dirty trick: access to $ActionFileDefaultTemplate value */
#include "cfsysline.h"
#include "module-template.h"
#include "conf.h"
#include "errmsg.h"

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("ompipe")

/* internal structures
 */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)


/* globals for default values */
/* end globals for default values */


typedef struct _instanceData {
	uchar	*pipe;	/* pipe or template name (display only) */
	uchar	*tplName;       /* format template to use */
	short	fd;		/* pipe descriptor for (current) pipe */
	sbool	bHadError;	/* did we already have/report an error on this pipe? */
} instanceData;

typedef struct configSettings_s {
	EMPTY_STRUCT
} configSettings_t;
static configSettings_t __attribute__((unused)) cs;

/* tables for interfacing with the v6 config system */
/* action (instance) parameters */
static struct cnfparamdescr actpdescr[] = {
	{ "pipe", eCmdHdlrString, CNFPARAM_REQUIRED },
	{ "template", eCmdHdlrGetWord, 0 }
};
static struct cnfparamblk actpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(actpdescr)/sizeof(struct cnfparamdescr),
	  actpdescr
	};

BEGINinitConfVars		/* (re)set config variables to default values */
CODESTARTinitConfVars 
ENDinitConfVars


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	dbgprintf("pipe %s", pData->pipe);
	if (pData->fd == -1)
		dbgprintf(" (unused)");
ENDdbgPrintInstInfo


/* This is now shared code for all types of files. It simply prepares
 * pipe access, which, among others, means the the pipe wil be opened
 * and any directories in between will be created (based on config, of
 * course). -- rgerhards, 2008-10-22
 * changed to iRet interface - 2009-03-19
 */
static inline rsRetVal
preparePipe(instanceData *pData)
{
	DEFiRet;
	pData->fd = open((char*) pData->pipe, O_RDWR|O_NONBLOCK|O_CLOEXEC);
	if(pData->fd < 0 ) {
		pData->fd = -1;
		if(!pData->bHadError) {
			char errStr[1024];
			rs_strerror_r(errno, errStr, sizeof(errStr));
			errmsg.LogError(0, RS_RET_NO_FILE_ACCESS, "Could no open output pipe '%s': %s",
				        pData->pipe, errStr);
			pData->bHadError = 1;
		}
		DBGPRINTF("Error opening log pipe: %s\n", pData->pipe);
	}
	RETiRet;
}


/* rgerhards 2004-11-11: write to a pipe output. This
 * will be called for all outputs using pipe semantics,
 * for example also for pipes.
 */
static rsRetVal writePipe(uchar **ppString, instanceData *pData)
{
	int iLenWritten;
	DEFiRet;

	ASSERT(pData != NULL);

	if(pData->fd == -1) {
		rsRetVal iRetLocal;
		iRetLocal = preparePipe(pData);
		if((iRetLocal != RS_RET_OK) || (pData->fd == -1))
			ABORT_FINALIZE(RS_RET_SUSPENDED); /* whatever the failure was, we need to retry */
	}

	/* create the message based on format specified */
	iLenWritten = write(pData->fd, ppString[0], strlen((char*)ppString[0]));
	if(iLenWritten < 0) {
		int e = errno;
		char errStr[1024];
		rs_strerror_r(errno, errStr, sizeof(errStr));
		DBGPRINTF("pipe (%d) write error %d: %s\n", pData->fd, e, errStr);

		/* If a named pipe is full, we suspend this action for a while */
		if(e == EAGAIN)
			ABORT_FINALIZE(RS_RET_SUSPENDED);

		close(pData->fd);
		pData->fd = -1; /* tell that fd is no longer open! */
		iRet = RS_RET_SUSPENDED;
		errno = e;
		errmsg.LogError(0, NO_ERRCODE, "%s", pData->pipe);
	}

finalize_it:
	RETiRet;
}


BEGINcreateInstance
CODESTARTcreateInstance
	pData->pipe = NULL;
	pData->fd = -1;
	pData->bHadError = 0;
ENDcreateInstance


BEGINfreeInstance
CODESTARTfreeInstance
	free(pData->pipe);
	if(pData->fd != -1)
		close(pData->fd);
ENDfreeInstance


BEGINtryResume
CODESTARTtryResume
ENDtryResume

BEGINdoAction
CODESTARTdoAction
	DBGPRINTF(" (%s)\n", pData->pipe);
	iRet = writePipe(ppString, pData);
ENDdoAction


static inline void
setInstParamDefaults(instanceData *pData)
{
	pData->tplName = NULL;
}

BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
CODESTARTnewActInst
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	CHKiRet(createInstance(&pData));
	setInstParamDefaults(pData);

	CODE_STD_STRING_REQUESTparseSelectorAct(1)
	for(i = 0 ; i < actpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "pipe")) {
			pData->pipe = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "template")) {
			pData->tplName = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else {
			dbgprintf("ompipe: program error, non-handled "
			  "param '%s'\n", actpblk.descr[i].name);
		}
	}

	if(pData->tplName == NULL) {
		CHKiRet(OMSRsetEntry(*ppOMSR, 0, (uchar*) "RSYSLOG_FileFormat",
			OMSR_NO_RQD_TPL_OPTS));
	} else {
		CHKiRet(OMSRsetEntry(*ppOMSR, 0,
			(uchar*) strdup((char*) pData->tplName),
			OMSR_NO_RQD_TPL_OPTS));
	}
CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst

BEGINparseSelectorAct
CODESTARTparseSelectorAct
	/* yes, the if below is redundant, but I need it now. Will go away as
	 * the code further changes.  -- rgerhards, 2007-07-25
	 */
	if(*p == '|') {
		if((iRet = createInstance(&pData)) != RS_RET_OK) {
			ENDfunc
			return iRet; /* this can not use RET_iRet! */
		}
	} else {
		/* this is not clean, but we need it for the time being
		 * TODO: remove when cleaning up modularization 
		 */
		ENDfunc
		return RS_RET_CONFLINE_UNPROCESSED;
	}

	CODE_STD_STRING_REQUESTparseSelectorAct(1)
	CHKmalloc(pData->pipe = malloc(512));
	++p;
	CHKiRet(cflineParseFileName(p, (uchar*) pData->pipe, *ppOMSR, 0, OMSR_NO_RQD_TPL_OPTS,
				       (pszFileDfltTplName == NULL) ? (uchar*)"RSYSLOG_FileFormat" : pszFileDfltTplName));
		
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINdoHUP
CODESTARTdoHUP
	if(pData->fd != -1) {
		close(pData->fd);
		pData->fd = -1;
	}
ENDdoHUP


BEGINmodExit
CODESTARTmodExit
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_doHUP
CODEqueryEtryPt_STD_CONF2_CNFNAME_QUERIES 
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit(Pipe)
CODESTARTmodInit
INITLegCnfVars
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
ENDmodInit
/* vi:set ai:
 */
