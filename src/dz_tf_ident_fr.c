/* DZco - digital control library
 * Copyright (C) 2000 Tomomichi Sugihara (Zhidao)
 *
 * dz_tf_ident_fr - transfer function: identification from frequency response
 */

#include <dzco/dz_tf.h>

/* ********************************************************** */
/* frequency response
 * ********************************************************** */

zComplex *dzFreqResToComplex(dzFreqRes *fr, zComplex *c, double *af)
{
  zComplexCreatePolar( c, pow( 10, 0.05*fr->g ), zDeg2Rad(fr->p) );
  *af = zPIx2 * fr->f;
  return c;
}

dzFreqRes *dzFreqResFromComplex(dzFreqRes *fr, zComplex *c, double *af)
{
  fr->f = *af / zPIx2;
  fr->g = 20 * log10( zComplexAbs(c) );
  fr->p = zRad2Deg( zComplexArg(c) );
  return fr;
}

dzFreqRes *dzFreqResConnectTF(dzFreqRes *frin, dzTF *tf, dzFreqRes *frout)
{
  zComplex gin, cg, gout;
  double af;

  dzFreqResToComplex( frin, &gin, &af );
  dzTFFreqRes( tf, af, &cg );
  zComplexCMul( &cg, &gin, &gout );
  dzFreqResFromComplex( frout, &gout, &af );
  return frout;
}

dzFreqRes *dzFreqRes2Closed(dzFreqRes *frin, dzFreqRes *frout)
{
  zComplex gin, gin1, gout;
  double af;

  dzFreqResToComplex( frin, &gin, &af );
  zComplexCreate( &gin1, 1+gin.re, gin.im );
  zComplexCDiv( &gin, &gin1, &gout );
  dzFreqResFromComplex( frout, &gout, &af );
  return frout;
}

dzFreqRes *dzFreqRes2Open(dzFreqRes *frin, dzFreqRes *frout)
{
  zComplex gin, gin1, gout;
  double af;

  dzFreqResToComplex( frin, &gin, &af );
  zComplexCreate( &gin1, 1-gin.re, -gin.im );
  zComplexCDiv( &gin, &gin1, &gout );
  dzFreqResFromComplex( frout, &gout, &af );
  return frout;
}

/* ********************************************************** */
/* list of sampled frequency responses
 * ********************************************************** */

#define DZ_FREQRESLIST_CONV( __conv ) \
  dzFreqResListCell *cpin, *cpout;\
  zListInit( outlist );\
  zListForEach( inlist, cpin ){\
    if( !( cpout = zAlloc( dzFreqResListCell, 1 ) ) ) break;\
    __conv;\
    zListInsertHead( outlist, cpout );\
  }\
  return zListSize(outlist)

int dzFreqResList2Closed(dzFreqResList *inlist, dzFreqResList *outlist)
{
  DZ_FREQRESLIST_CONV( dzFreqRes2Closed( &cpin->data, &cpout->data ) );
}

int dzFreqResList2Open(dzFreqResList *inlist, dzFreqResList *outlist)
{
  DZ_FREQRESLIST_CONV( dzFreqRes2Open( &cpin->data, &cpout->data ) );
}

int dzFreqResListConnectTF(dzFreqResList *inlist, dzTF *tf, dzFreqResList *outlist)
{
  DZ_FREQRESLIST_CONV( dzFreqResConnectTF( &cpin->data, tf, &cpout->data ) );
}

int dzFreqResListFScan(FILE *fp, dzFreqResList *list, double fmin, double fmax)
{
  dzFreqRes fr;
  dzFreqResListCell *cp;

  zListInit( list );
  while( !feof( fp ) ){
    if( fscanf( fp, "%lf %lf %lf", &fr.f, &fr.g, &fr.p ) != 3 ) break;
    if( fr.f < fmin || fr.f > fmax ) continue;
    if( !( cp = zAlloc( dzFreqResListCell, 1 ) ) ) break;
    memcpy( &cp->data, &fr, sizeof(dzFreqRes) );
    zListInsertHead( list, cp );
  }
  return zListSize( list );
}

int dzFreqResListFPrint(FILE *fp, dzFreqResList *list, double fmin, double fmax)
{
  dzFreqResListCell *cp;
  register int count = 0;

  zListForEach( list, cp ){
    if( cp->data.f < fmin || cp->data.f > fmax ) continue;
    fprintf( fp, "%.10g %.10g %.10g\n", cp->data.f, cp->data.g, cp->data.p );
    count++;
  }
  return count;
}

int dzFreqResListScanFile(dzFreqResList *list, char filename[], double fmin, double fmax)
{
  FILE *fp;

  if( !( fp = fopen( filename, "r" ) ) ){
    ZOPENERROR( filename );
    return 0;
  }
  dzFreqResListFScan( fp, list, fmin, fmax );
  fclose( fp );
  return zListSize(list);
}

int dzFreqResListPrintFile(dzFreqResList *list, char filename[], double fmin, double fmax)
{
  FILE *fp;
  int count;

  if( !( fp = fopen( filename, "w" ) ) ){
    ZOPENERROR( filename );
    return 0;
  }
  count = dzFreqResListFPrint( fp, list, fmin, fmax );
  fclose( fp );
  return count;
}

/* identification of a transfer function from frequency response */

typedef struct{
  int ns; /* number of samples */
  int nn; /* dimension of numerator */
  int nd; /* dimension of denominator */
  int ndim; /* larger number of nn and nd */
  zCVec freq_res; /* sampled frequency responses */
  zVec ang_freq; /* sampled angular frequencies */
  zVec mag; /* inverse of currently estimated denominator */
  zVec phi; /* parameters to be identified */
  zVecArray ps_re, ps_im, fr_re, fr_im; /* power series of sampled complex angular frequencies */
  /* the following members are only used for iteration */
  zMat q; /* coefficient matrix for least-square solution */
  zVec p; /* vector for least-square solution */
  zVec phi_prev; /* parameters identified in the previous step */
  zVec xr, xi; /* intermediate vectors to compute matrix and vector for least-square solution */
  double gr, gi; /* intermedial values to compute matrix and vector for least-square solution */
} dzFreqResIdentData;

static bool _dzFreqResIdentDataAlloc(dzFreqResIdentData *fri, int nn, int nd, int size)
{
  fri->ns = size;
  fri->nn = nn;
  fri->nd = nd;
  fri->ndim = zMax( nn, nd );
  fri->freq_res = zCVecAlloc( fri->ns );
  fri->ang_freq = zVecAlloc( fri->ns );
  fri->mag = zVecAlloc( fri->ns );
  fri->phi = zVecAlloc( fri->nn + fri->nd + 1 );

  zVecArrayAlloc( &fri->ps_re, fri->ndim+1, fri->ns );
  zVecArrayAlloc( &fri->ps_im, fri->ndim+1, fri->ns );
  zVecArrayAlloc( &fri->fr_re, fri->ndim+1, fri->ns );
  zVecArrayAlloc( &fri->fr_im, fri->ndim+1, fri->ns );
  return fri->freq_res && fri->ang_freq && fri->mag && fri->phi &&
         zArraySize(&fri->ps_re) != 0 &&
         zArraySize(&fri->ps_im) != 0 &&
         zArraySize(&fri->fr_re) != 0 &&
         zArraySize(&fri->fr_im) != 0 ? true : false;
}

static bool _dzFreqResIdentDataLSMAlloc(dzFreqResIdentData *fri)
{
  fri->q = zMatAllocSqr( zVecSizeNC(fri->phi) );
  fri->p = zVecAlloc( zVecSizeNC(fri->phi) );
  fri->phi_prev = zVecAlloc( zVecSizeNC(fri->phi) );
  fri->xr = zVecAlloc( zVecSizeNC(fri->phi) );
  fri->xi = zVecAlloc( zVecSizeNC(fri->phi) );
  return fri->q && fri->p && fri->phi_prev && fri->xr && fri->xi ? true: false;
}

static void _dzFreqResIdentDataFree(dzFreqResIdentData *fri)
{
  zCVecFree( fri->freq_res );
  zVecFree( fri->ang_freq );
  zVecFree( fri->mag );
  zVecFree( fri->phi );
  zVecArrayFree( &fri->ps_re );
  zVecArrayFree( &fri->ps_im );
  zVecArrayFree( &fri->fr_re );
  zVecArrayFree( &fri->fr_im );
}

static void _dzFreqResIdentDataLSMFree(dzFreqResIdentData *fri)
{
  zMatFree( fri->q );
  zVecFree( fri->p );
  zVecFree( fri->phi_prev );
  zVecFree( fri->xr );
  zVecFree( fri->xi );
}

static int _dzFreqResIdentDataRead(dzFreqResIdentData *fri, dzFreqResList *list, int nn, int nd)
{
  dzFreqResListCell *cp;
  double omega, omegaj;
  zComplex iomegaj, c;
  register int j, k = 0;

  if( !_dzFreqResIdentDataAlloc( fri, nn, nd, zListSize(list) ) ) return 0;
  zListForEach( list, cp ){
    dzFreqResToComplex( &cp->data, zCVecElemNC(fri->freq_res,k), &omega );
    zVecSetElemNC( fri->ang_freq, k, omega );
    zVecSetElemNC( fri->mag, k, 1.0 );
    for( j=0; j<=fri->ndim; j++ ){
      omegaj = pow( omega, j );
      if( zIsEven(j) )
        zComplexCreate( &iomegaj, zIsEven(j/2) ? omegaj : -omegaj, 0 );
      else
        zComplexCreate( &iomegaj, 0, zIsEven((j-1)/2) ? omegaj : -omegaj );
      zVecArrayElem(&fri->ps_re,k,j) = iomegaj.re;
      zVecArrayElem(&fri->ps_im,k,j) = iomegaj.im;
      zComplexCMul( zCVecElemNC(fri->freq_res,k), &iomegaj, &c );
      zVecArrayElem(&fri->fr_re,k,j) = c.re;
      zVecArrayElem(&fri->fr_im,k,j) = c.im;
    }
    k++;
  }
  return fri->ns;
}

static void _dzFreqResIdentDataLSMCreate(dzFreqResIdentData *fri)
{
  register int j, k;

  zMatZero( fri->q );
  zVecZero( fri->p );
  for( k=0; k<fri->ns; k++ ){
    for( j=0; j<=fri->nn; j++ ){
      zVecElemNC(fri->xr,j) = zVecArrayElem(&fri->ps_re,k,j) * zVecElemNC(fri->mag,k);
      zVecElemNC(fri->xi,j) = zVecArrayElem(&fri->ps_im,k,j) * zVecElemNC(fri->mag,k);
    }
    for( j=1; j<=fri->nd; j++ ){
      zVecElemNC(fri->xr,fri->nn+j) = -zVecArrayElem(&fri->fr_re,k,j) * zVecElemNC(fri->mag,k);
      zVecElemNC(fri->xi,fri->nn+j) = -zVecArrayElem(&fri->fr_im,k,j) * zVecElemNC(fri->mag,k);
    }
    fri->gr = zVecArrayElem(&fri->fr_re,k,0) * zVecElemNC(fri->mag,k);
    fri->gi = zVecArrayElem(&fri->fr_im,k,0) * zVecElemNC(fri->mag,k);
    zMatAddDyadNC( fri->q, fri->xr, fri->xr );
    zMatAddDyadNC( fri->q, fri->xi, fri->xi );
    zVecCatDRC( fri->p, fri->gr, fri->xr );
    zVecCatDRC( fri->p, fri->gi, fri->xi );
  }
}

static void _dzFreqResIdentDataUpdateMag(dzFreqResIdentData *fri)
{
  register int j, k;
  double rr, ri;

  for( k=0; k<fri->ns; k++ ){
    rr = zVecArrayElem(&fri->ps_re,k,0);
    ri = zVecArrayElem(&fri->ps_im,k,0);
    for( j=1; j<=fri->nd; j++ ){
      rr += zVecArrayElem(&fri->ps_re,k,j)*zVecElemNC(fri->phi,fri->nn+j);
      ri += zVecArrayElem(&fri->ps_im,k,j)*zVecElemNC(fri->phi,fri->nn+j);
    }
    zVecElemNC(fri->mag,k) = 1.0 / ( rr*rr + ri*ri );
  }
}

static bool _dzFreqResIdent(dzFreqResIdentData *fri, dzFreqResList *list, int nn, int nd, int iter)
{
  double dist, dist_prev;
  register int i;

  if( _dzFreqResIdentDataRead( fri, list, nn, nd ) == 0 ) return false;
  if( !_dzFreqResIdentDataLSMAlloc( fri ) ) return false;
  ZITERINIT( iter );
  zVecSetAll( fri->phi_prev, HUGE_VAL );
  for( dist_prev=HUGE_VAL, i=0; i<iter; i++ ){
    _dzFreqResIdentDataLSMCreate( fri );
    zLESolveGauss( fri->q, fri->p, fri->phi );
    dist = zVecDist( fri->phi, fri->phi_prev );
    if( zIsTiny( dist - dist_prev ) ) break;
    _dzFreqResIdentDataUpdateMag( fri );
    zVecCopyNC( fri->phi, fri->phi_prev );
    dist_prev = dist;
  }
  _dzFreqResIdentDataLSMFree( fri );
  return true;
}

dzTF *dzTFIdentFromFreqRes(dzTF *tf, dzFreqResList *list, int nn, int nd, int iter)
{
  dzFreqResIdentData fri;
  register int i;

  if( !_dzFreqResIdent( &fri, list, nn, nd, iter ) ) return NULL;
  if( !dzTFAlloc( tf, nn, nd ) ) return NULL;
  for( i=0; i<=fri.nn; i++ )
    zPexSetCoeff( dzTFNum(tf), i, zVecElemNC(fri.phi,i) );
  zPexSetCoeff( dzTFDen(tf), 0, 1.0 );
  for( i=1; i<=fri.nd; i++ )
    zPexSetCoeff( dzTFDen(tf), i, zVecElemNC(fri.phi,fri.nn+i) );
  _dzFreqResIdentDataFree( &fri );
  return tf;
}