/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tq.h"

static int32_t createStreamRunReq(SStreamMeta* pStreamMeta, bool* pScanIdle);

// this function should be executed by stream threads.
// extract submit block from WAL, and add them into the input queue for the sources tasks.
int32_t tqStreamTasksScanWal(STQ* pTq) {
  int32_t      vgId = TD_VID(pTq->pVnode);
  SStreamMeta* pMeta = pTq->pStreamMeta;
  int64_t      st = taosGetTimestampMs();

  while (1) {
    int32_t scan = pMeta->walScanCounter;
    tqDebug("vgId:%d continue check if data in wal are available, walScanCounter:%d", vgId, scan);

    // check all restore tasks
    bool shouldIdle = true;
    createStreamRunReq(pTq->pStreamMeta, &shouldIdle);

    int32_t times = 0;

    if (shouldIdle) {
      taosWLockLatch(&pMeta->lock);

      pMeta->walScanCounter -= 1;
      times = pMeta->walScanCounter;

      ASSERT(pMeta->walScanCounter >= 0);

      if (pMeta->walScanCounter <= 0) {
        taosWUnLockLatch(&pMeta->lock);
        break;
      }

      taosWUnLockLatch(&pMeta->lock);
      tqDebug("vgId:%d scan wal for stream tasks for %d times", vgId, times);
    }
  }

  int64_t el = (taosGetTimestampMs() - st);
  tqDebug("vgId:%d scan wal for stream tasks completed, elapsed time:%"PRId64" ms", vgId, el);
  return 0;
}

int32_t createStreamRunReq(SStreamMeta* pStreamMeta, bool* pScanIdle) {
  *pScanIdle = true;
  bool    noNewDataInWal = true;
  int32_t vgId = pStreamMeta->vgId;

  int32_t numOfTasks = taosArrayGetSize(pStreamMeta->pTaskList);
  if (numOfTasks == 0) {
    return TSDB_CODE_SUCCESS;
  }

  SArray* pTaskList = NULL;
  taosWLockLatch(&pStreamMeta->lock);
  pTaskList = taosArrayDup(pStreamMeta->pTaskList, NULL);
  taosWUnLockLatch(&pStreamMeta->lock);

  tqDebug("vgId:%d start to check wal to extract new submit block for %d tasks", vgId, numOfTasks);

  // update the new task number
  numOfTasks = taosArrayGetSize(pTaskList);

  for (int32_t i = 0; i < numOfTasks; ++i) {
    int32_t*     pTaskId = taosArrayGet(pTaskList, i);
    SStreamTask* pTask = streamMetaAcquireTask(pStreamMeta, *pTaskId);
    if (pTask == NULL) {
      continue;
    }

    int32_t status = pTask->status.taskStatus;
    if (pTask->taskLevel != TASK_LEVEL__SOURCE) {
      tqDebug("s-task:%s not source task, no need to start", pTask->id.idStr);
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    if (streamTaskShouldStop(&pTask->status) || status == TASK_STATUS__RECOVER_PREPARE ||
        status == TASK_STATUS__WAIT_DOWNSTREAM) {
      tqDebug("s-task:%s not ready for new submit block from wal, status:%d", pTask->id.idStr, status);
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    if (tInputQueueIsFull(pTask)) {
      tqDebug("vgId:%d s-task:%s input queue is full, do nothing", vgId, pTask->id.idStr);
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    *pScanIdle = false;

    // seek the stored version and extract data from WAL
    int32_t code = walReadSeekVer(pTask->exec.pWalReader, pTask->chkInfo.currentVer);
    if (code != TSDB_CODE_SUCCESS) {  // no data in wal, quit
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    // append the data for the stream
    tqDebug("vgId:%d s-task:%s wal reader seek to ver:%" PRId64, vgId, pTask->id.idStr, pTask->chkInfo.currentVer);

    SPackedData packData = {0};
    code = extractSubmitMsgFromWal(pTask->exec.pWalReader, &packData);
    if (code != TSDB_CODE_SUCCESS) {  // failed, continue
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    SStreamDataSubmit2* p = streamDataSubmitNew(packData, STREAM_INPUT__DATA_SUBMIT);
    if (p == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      tqError("%s failed to create data submit for stream since out of memory", pTask->id.idStr);
      streamMetaReleaseTask(pStreamMeta, pTask);
      continue;
    }

    noNewDataInWal = false;

    code = tqAddInputBlockNLaunchTask(pTask, (SStreamQueueItem*)p, packData.ver);
    if (code == TSDB_CODE_SUCCESS) {
      pTask->chkInfo.currentVer = walReaderGetCurrentVer(pTask->exec.pWalReader);
      tqDebug("s-task:%s set the ver:%" PRId64 " from WALReader after extract block from WAL", pTask->id.idStr,
              pTask->chkInfo.currentVer);
    } else {
      tqError("s-task:%s append input queue failed, ver:%"PRId64, pTask->id.idStr, pTask->chkInfo.currentVer);
    }

    streamDataSubmitDestroy(p);
    taosFreeQitem(p);
    streamMetaReleaseTask(pStreamMeta, pTask);
  }

  // all wal are checked, and no new data available in wal.
  if (noNewDataInWal) {
    *pScanIdle = true;
  }

  taosArrayDestroy(pTaskList);
  return 0;
}

