
# include "sqlite3.h"

# include "sqlite3ext.h"
  SQLITE_EXTENSION_INIT1

#include <cstring>

#include <faiss/impl/ProductQuantizer.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/VectorTransform.h>
#include <faiss/IndexPreTransform.h>

#include <faiss/utils/utils.h>

extern "C" {
  static void vec1FaissTrainStep(
      sqlite3_context *pCtx, 
      int nArg, 
      sqlite3_value **aArg
  );
  static void vec1FaissTrainFinal(sqlite3_context *pCtx);
}

typedef unsigned char u8;
typedef sqlite3_int64 i64;
typedef unsigned int u32;

/*************************************************************************
*/

/*
** Value for Vec1ModelHeader.iVersion.
*/
#define VEC1_HEADER_VERSION     (0*1000 + 4)

/*
** The two distance metrics supported. 
*/
#define VEC1_DISTANCE_L2   1
#define VEC1_DISTANCE_COS  2

#define VEC1_HEADER_SIZE (6*sizeof(u32))

#define VEC1_MODEL_INDEX    0x01
#define VEC1_MODEL_ROTATE   0x02
#define VEC1_MODEL_RESIDUAL 0x04

static void vec1PutU32(u8 *aBuf, u32 val){
  aBuf[0] = (val >> 24) & 0xFF;
  aBuf[1] = (val >> 16) & 0xFF;
  aBuf[2] = (val >>  8) & 0xFF;
  aBuf[3] = (val >>  0) & 0xFF;
}

/*
** Write a model header into buffer aBuf[].
*/
static void vec1HeaderWrite(
  u8 *aBuf, 
  int flags,
  int nElem,
  int nCodebook,
  int nBucket,
  int eDistance
){
  vec1PutU32(&aBuf[0], VEC1_HEADER_VERSION);
  vec1PutU32(&aBuf[4], flags);
  vec1PutU32(&aBuf[8], nElem);
  vec1PutU32(&aBuf[12], nCodebook);
  vec1PutU32(&aBuf[16], nBucket);
  vec1PutU32(&aBuf[20], eDistance);
}

/*
*************************************************************************/


/*
** Context object for the vec1_faiss_train() aggregate function.
*/
struct FaissTrainCtx {

  /* Data */
  u8 *aData;
  i64 nData;
  i64 nAlloc;

  /* Training parameters */
  int nElem;                      /* Number of elements in each vector */
  int nCodebook;                  /* Number of vector sub-sections */
  int nBucket;                    /* Number of IVF buckets */
  int bOpq;                       /* Use OPQ rotation */
  int eDistance;
};

/*
** Set the error message in context object pCtx to the result of formatting
** zFmt printf() style.
*/
static void vec1ResultErrorF(
  sqlite3_context *pCtx,          /* Context to set error in */
  const char *zFmt,               /* printf() style format for error message */
  ...                             /* Trailing arguments for printf() */
){
  char *zMsg = 0;
  va_list ap;
  va_start(ap, zFmt);
  zMsg = sqlite3_vmprintf(zFmt, ap);
  va_end(ap);
  sqlite3_result_error(pCtx, zMsg, -1);
  sqlite3_free(zMsg);
}

#define VEC1_PQ_DEFAULT_N_CODEBOOK   16

#define VEC1_PQ_MIN_N_CODEBOOK        8
#define VEC1_PQ_MAX_N_CODEBOOK      128

#define VEC1_PQ_MIN_N_BUCKET         32
#define VEC1_PQ_MAX_N_BUCKET       4096

/*
** This function is used to parse the small json objects used to specify
** configuration options to components in this module.
*/
static int vec1ParseJsonConfig(
  sqlite3 *db,
  const char *zJson,
  int (*x)(void*, const char *, double, const char *, char **),
  void *pCtx,
  char **pz
){
  sqlite3_stmt *pStmt = 0;
  int rc = SQLITE_OK;
  int rc2 = SQLITE_OK;

  rc = sqlite3_prepare(
      db, "SELECT key, value FROM json_each(?)", -1, &pStmt, 0
  );

  if( rc==SQLITE_OK ){
    rc = sqlite3_bind_text(pStmt, 1, zJson, -1, SQLITE_STATIC);
  }
  while( rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pStmt) ){
    const char *zKey = (const char*)sqlite3_column_text(pStmt, 0);
    const char *zVal = 0;
    double fVal = 0;

    switch( sqlite3_column_type(pStmt, 1) ){
      case SQLITE_INTEGER:
      case SQLITE_FLOAT:
        fVal = sqlite3_column_double(pStmt, 1);
        break;

      case SQLITE_TEXT:
        zVal = (const char*)sqlite3_column_text(pStmt, 1);
        break;

      default:
        *pz = sqlite3_mprintf("unexpected type for config element: %s", zKey);
        rc = SQLITE_ERROR;
        break;
    }

    if( rc==SQLITE_OK ){
      rc = x(pCtx, zKey, fVal, zVal, pz);
    }
  }

  rc2 = sqlite3_finalize(pStmt);
  if( rc==SQLITE_OK && rc2!=SQLITE_OK ){
    rc = rc2;
    *pz = sqlite3_mprintf("%s", sqlite3_errmsg(db));
  }

  return rc;
}

static int vec1Ann1TrainCfg(
  void *pCtx, 
  const char *zOpt, 
  double fVal, 
  const char *zVal,
  char **pzErr
){
  int rc = SQLITE_OK;
  FaissTrainCtx *p = (FaissTrainCtx*)pCtx;
  i64 iVal = (i64)fVal;

  if( sqlite3_stricmp("codesize", zOpt)==0 ){
    if( iVal && (iVal<VEC1_PQ_MIN_N_CODEBOOK || iVal>VEC1_PQ_MAX_N_CODEBOOK) ){
      *pzErr = sqlite3_mprintf(
          "parameter codesize must be 0 or between %d and %d",
          VEC1_PQ_MIN_N_CODEBOOK, VEC1_PQ_MAX_N_CODEBOOK
      );
      rc = SQLITE_ERROR;
    }else{
      p->nCodebook = (int)iVal;
    }
  }else 
  if( sqlite3_stricmp("nbucket", zOpt)==0 ){
    if( iVal!=0 && (iVal<VEC1_PQ_MIN_N_BUCKET || iVal>VEC1_PQ_MAX_N_BUCKET) ){
      *pzErr = sqlite3_mprintf(
          "parameter nbucket must be 0 or between %d and %d",
          VEC1_PQ_MIN_N_BUCKET, VEC1_PQ_MAX_N_BUCKET
      );
      rc = SQLITE_ERROR;
    }else{
      p->nBucket = (int)iVal;
    }
  }else
  if( sqlite3_stricmp("opq", zOpt)==0 ){
    p->bOpq = ((int)iVal)!=0;
  }else if( sqlite3_stricmp("nthread", zOpt)==0 ){
  }else if( sqlite3_stricmp("distance", zOpt)==0 ){
    if( sqlite3_stricmp(zVal, "cos")==0 ){
      p->eDistance = VEC1_DISTANCE_COS;
    }else if( sqlite3_stricmp(zVal, "l2")==0 ){
      p->eDistance = VEC1_DISTANCE_L2;
    }else{
      *pzErr = sqlite3_mprintf("unknown distance: %s", zVal);
      rc = SQLITE_ERROR;
    }
  }else if( sqlite3_stricmp("svd_verify", zOpt)==0 ){
  }else if( sqlite3_stricmp("progress", zOpt)==0 ){
  }else{
    *pzErr = sqlite3_mprintf("unknown parameter: %s", zOpt);
    rc = SQLITE_ERROR;
  }

  return rc;
}

static void vec1FaissTrainStep(
  sqlite3_context *pCtx, 
  int nArg, 
  sqlite3_value **aArg
){
  FaissTrainCtx *p = 0;
  int nBlob = sqlite3_value_bytes(aArg[0]);
  const u8 *pBlob = (const u8*)sqlite3_value_blob(aArg[0]);

  p = (FaissTrainCtx*)sqlite3_aggregate_context(pCtx, sizeof(*p));
  if( !p ) return;

  if( (p->nElem!=0 && (int)(p->nElem*sizeof(float))!=nBlob)
   || (nBlob % sizeof(float))!=0
  ){
    vec1ResultErrorF(pCtx, "unexpected blob size: %s", nBlob);
    return;
  }

  if( p->nElem==0 ){
    p->nElem = nBlob / sizeof(float);
    p->nCodebook = 16;
    p->nBucket = 0;
    if( nArg==2 ){
      int rc = SQLITE_OK;
      char *zErr = 0;
      sqlite3 *db = sqlite3_context_db_handle(pCtx);
      const char *zJson = (const char*)sqlite3_value_text(aArg[1]);
      rc = vec1ParseJsonConfig(db, zJson, vec1Ann1TrainCfg, (void*)p, &zErr);
      if( rc!=SQLITE_OK ){
        vec1ResultErrorF(pCtx, "%s", zErr);
        sqlite3_free(zErr);
        return;
      }
    }
  }

  if( p->nData+nBlob>p->nAlloc ){
    i64 nNew = (p->nAlloc==0 ? 1024*1024 : p->nAlloc*2);
    p->aData = (u8*)sqlite3_realloc64((void*)p->aData, nNew);
    if( p->aData==0 ){
      sqlite3_result_error_nomem(pCtx);
      return;
    }
    p->nAlloc = nNew;
  }

  memcpy(&p->aData[p->nData], pBlob, nBlob);
  p->nData += nBlob;
}

/*
** Normalize nElem dimension vector aElem. 
**
** Assume a vector with magnitude 0.0 normalizes to itself. 
*/
static void vec1NormalizeVector(float *aElem, int nElem){
  int ii;
  double fSum = 0.0;
  for(ii=0; ii<nElem; ii++){
    fSum += (aElem[ii] * aElem[ii]);
  }
  if( fSum==0.0 ) return;
  fSum = 1.0 / sqrt(fSum);
  for(ii=0; ii<nElem; ii++){
    aElem[ii] = (float)(aElem[ii] * fSum);
  }
}

/*
** Normalize all the training vectors.
*/
static void vec1TrainNormalizeAll(FaissTrainCtx *p){
  int ii;
  int nBytePerVector = p->nElem * sizeof(float);
  int nVec = p->nData / nBytePerVector;
  for(ii=0; ii<nVec; ii++){
    vec1NormalizeVector((float*)&p->aData[ii*nBytePerVector], p->nElem);
  }
}

#define VEC1_TRAINING_SET_MIN 1000
#define MAX(a,b) ((a)>(b) ? (a) : (b));

static void vec1FaissTrainFinal(sqlite3_context *pCtx){
  FaissTrainCtx *p = 0;

  /* Check that sufficient training vectors were provided. */
  p = (FaissTrainCtx*)sqlite3_aggregate_context(pCtx, sizeof(*p));

  if( p && p->nData>0 ){
    int nVector = p->nData / (p->nElem * sizeof(float));
    const float *aVector = (const float*)p->aData;
    int flags = VEC1_MODEL_INDEX 
              | (p->bOpq ? VEC1_MODEL_ROTATE : 0)
              | (p->nCodebook>0 ? VEC1_MODEL_RESIDUAL : 0);

    if( nVector<VEC1_TRAINING_SET_MIN ){
      vec1ResultErrorF(pCtx, 
          "too few training vectors (require %d, have %d)",
          VEC1_TRAINING_SET_MIN, nVector
      );
      sqlite3_free(p->aData);
      return;
    }

    if( p->nCodebook==0 ) p->bOpq = 0;
    if( p->eDistance==VEC1_DISTANCE_COS ){
      vec1TrainNormalizeAll(p);
    }

    try {

      faiss::OPQMatrix *pOpq = 0;
      faiss::IndexPQ *pPqIdx = 0;
      faiss::IndexIVFPQ *pIvfPqIdx = 0;
      faiss::IndexFlatL2 *pFlatL2Idx = 0;
      faiss::IndexIVFFlat *pIvfIdx = 0;
      faiss::IndexPreTransform *pTransformIdx = 0;

      faiss::Index *pIdx = 0;
      std::vector<float> *pPqData = 0;

      if( p->nCodebook>0 && p->nBucket>0 ){
        pFlatL2Idx = new faiss::IndexFlatL2(p->nElem);
        pIvfPqIdx = new faiss::IndexIVFPQ(
            pFlatL2Idx, p->nElem, p->nBucket, p->nCodebook, 8
        );
        pIdx = (faiss::Index*)pIvfPqIdx;
        pPqData = &pIvfPqIdx->pq.centroids;
      }
      else if( p->nCodebook ){
        pPqIdx = new faiss::IndexPQ(p->nElem, p->nCodebook, 8);
        pIdx = (faiss::Index*)pPqIdx;
        pPqData = &pPqIdx->pq.centroids;
      }else{
        pFlatL2Idx = new faiss::IndexFlatL2(p->nElem);
        pIvfIdx = new faiss::IndexIVFFlat(pFlatL2Idx, p->nElem, p->nBucket);
        pIdx = (faiss::Index*)pIvfIdx;
      }

      if( p->bOpq ){
        pOpq = new faiss::OPQMatrix(p->nElem, p->nCodebook);
        pOpq->niter = 10;
        pOpq->niter_pq = 4;
        pOpq->niter_pq_0 = 5;

        pTransformIdx = new faiss::IndexPreTransform(pOpq, pIdx);
        pIdx = (faiss::Index*)pTransformIdx;
      }

      pIdx->train(nVector, aVector);

      std::vector<u8> buf;

      /* Serialize the model header into the buffer. */
      buf.resize(VEC1_HEADER_SIZE);
      vec1HeaderWrite(buf.data(), flags,
          p->nElem, p->nCodebook, p->nBucket, VEC1_DISTANCE_L2
      );

      if( pPqData ){
        const auto *p = reinterpret_cast<u8*>(pPqData->data());
        buf.insert(buf.end(), p, p + (pPqData->size() * sizeof(float)));
      }
      if( pFlatL2Idx ){
        const auto *z = reinterpret_cast<u8*>(pFlatL2Idx->get_xb());
        buf.insert(buf.end(), z, z + (p->nElem * p->nBucket * sizeof(float)));
      }
      if( p->bOpq ){
        const auto *p = reinterpret_cast<u8*>(pOpq->A.data());
        buf.insert(buf.end(), p, p + (pOpq->A.size() * sizeof(float)));
      }

      sqlite3_result_blob(pCtx, buf.data(), buf.size(), SQLITE_TRANSIENT);
   
      /* pOpq is now owned by pTransformIdx. pFlatL2Idx is owned by one
      ** or other of the pIvf* indexs. */
      // delete pOpq;             // Now owned by pTransformIdx
      // delete pFlatL2Idx;       // Owned by either pIvfPqIdx or pIvfIdx

      delete pPqIdx;
      delete pIvfPqIdx;
      delete pIvfIdx;
      delete pTransformIdx;
    }
    catch( const std::exception &e ){
      sqlite3_result_error(pCtx, e.what(), -1);
    }
  }

  if( p ) sqlite3_free(p->aData);
}

static int initExtension(sqlite3 *db, char **pzErr){
  int rc = SQLITE_OK;
  
  struct AggFunc {
    const char *zName;
    int nArg;
    void (*xStep)(sqlite3_context*,int,sqlite3_value**);
    void (*xFinal)(sqlite3_context*);
  } aAgg[] = {
    { "vec1_faiss_train", 1, vec1FaissTrainStep, vec1FaissTrainFinal },
    { "vec1_faiss_train", 2, vec1FaissTrainStep, vec1FaissTrainFinal },
  };
  int ii;

  for(ii=0; rc==SQLITE_OK && ii<(int)(sizeof(aAgg)/sizeof(aAgg[0])); ii++){
    struct AggFunc *p = &aAgg[ii];
    rc = sqlite3_create_function(
        db, p->zName, p->nArg, SQLITE_UTF8, 0, 0, p->xStep, p->xFinal
    );
  }

  return rc;
}

extern "C" {

SQLITE_API int sqlite3_vec1faiss_init(
    sqlite3 *db, 
    char **pzErrMsg, 
    const sqlite3_api_routines *pApi
){
#ifdef SQLITE_EXTENSION_INIT2
  SQLITE_EXTENSION_INIT2(pApi);
#endif

  return initExtension(db, pzErrMsg);
}

}
