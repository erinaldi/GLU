/*
    Copyright 2013 Renwick James Hudspith

    This file (Landau.c) is part of GLU.

    GLU is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GLU is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GLU.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
   @file Landau.c
   @brief Landau gauge fixing routines

  Landau gauge fixing code, malloc's the ffts and
  calculates psq. Then calls the FA code in gtrans.c
  If we do not reach adequate convergence #GF_GLU_FAILURES restart(s)
  with a random gauge transform is(are) performed.
 */

#include "Mainfile.h"

#include "FACG.h"          // Fourier-Accelerated Conjugate Gradient
#include "geometry.h"      // lattice geometry, used for psq
#include "gftests.h"       // derivative evaluations
#include "MAG.h"           // for randomly restarting the MAG
#include "plan_ffts.h"     // FFTW wrappers
#include "plaqs_links.h"   // average plaquette and link trace
#include "read_headers.h"  // understands header formats
#include "read_config.h"   // configuration reader
#include "random_config.h" // random config and lattice reunit 


// output the data, pass lat for the plaquette
static void
output_fixing_info( lat , theta , iters )
     struct site *__restrict lat ;
     const double theta ;
     const int iters ;
{
  // reunitarise just to limit the damage from round-off
  latt_reunitU( lat ) ;

  ////////// Print out the Gauge Fixing information /////////////

  printf( "[GF] Plaquette :: %1.15f \n[GF] Accuracy :: %1.4e\n" , 
	  av_plaquette( lat ) , theta ) ;
  GLU_real tr ;
  const double link = indivlinks( lat , &tr ) ;
  printf( "[GF] Iters :: %d\n[GF] Link trace :: %1.15f || Maximum :: %1.15f\n" , iters , link , tr / NC ) ; 
  double lin , log ;
  const_time( lat , &lin , &log ) ; 
  printf( "[GF] Temporal constance || Lin %e || Log %e \n" , lin , log ) ;
  gauge_functional( lat ) ;
  printf( "[GF] Functional :: %1.15f\n" , gauge_functional( lat ) ) ;

  ///////////////////////////////////////////////////////////////
  return ;
}
// printing helper ...
static void
print_failure_info( failure , iters ) 
     const int failure , iters ;
{
  if( failure < GF_GLU_FAILURES ) {
    printf( "\n[GF] Non-convergence ... Randomly-restarting \n\n[GF] Failure :: %d || Iters :: %d \n" 
	    , failure , iters ) ; 
  } else {
    printf( "\n[GF] Insufficient Convergence ......\n\n"
	    "[GF] Failures :: %d || Total iterations :: %d \n\n"
	    "[GF] -> Try reducing the tuning parameter OR/AND\n"
	    "increasing the maximum number of iterations -< \n\n" , failure , iters ) ; 
  }
  return ;
}

// cute little callback
static int 
( *FA_callback ) ( struct site *__restrict lat ,
		   GLU_complex *__restrict *__restrict gauge , 
		   GLU_complex *__restrict *__restrict out , 
		   GLU_complex *__restrict *__restrict in , 
		   const void *__restrict forward , 
		   const void *__restrict backward , 
		   const GLU_real *psq , 
		   double *tr ,
		   const double acc , 
		   const int max_iters ) ;

// callback selector
static void
select_callback( const int improvement ) 
{
  #ifdef LUXURY_GAUGE
  if( improvement != SMPREC_IMPROVE ) {
    FA_callback = luxury_copy_fast ; 
  } else { 
    #ifdef GLU_GFIX_SD
    FA_callback = FASD ; 
    #else
    FA_callback = FACG ; query_probes_Landau( ) ;
    #endif
  }
  #else
  if( improvement == SMPREC_IMPROVE ) {
    FA_callback = FASD_SMEAR ; 
  } else {
    #ifdef GLU_GFIX_SD
    FA_callback = FASD ;
    #else
    FA_callback = FACG ; query_probes_Landau( ) ;
    #endif
  } 
  #endif
  return ;
}

#ifdef HAVE_FFTW3_H // Fourier acceleration routines

// precomputatation of the lattice momentum
static void
precompute_momenta( psq ) 
     GLU_real *__restrict psq ;
{
  int i ;
  // factor due to the different momentum def 
#if ( defined deriv_linn ) || ( defined deriv_fulln ) 
  //const GLU_real factor = 1.0 / MAX_LANDAU ; //4.0 / 3.0 *
#elif defined deriv_fullnn
  //const GLU_real factor = 68.0 / 45.0 * MAX_LANDAU ; 
#endif
  #pragma omp parallel for private(i) shared(psq)
  PFOR(  i = 0 ; i < LVOLUME ; i++  ) {
    #if ( defined deriv_linn ) || ( defined deriv_fulln ) || ( defined deriv_fullnn )
    psq[i] = MAX_LANDAU / ( gen_p_sq_imp( i , ND )  ) ; 
    #else
    psq[i] = MAX_LANDAU / ( gen_p_sq( i , ND )  ) ; 
    #endif
  }
  return ;
}

#if ( defined LUXURY_GAUGE )

// fast routine used here
static int
luxury_copy_fast( lat , gauge , out , in , forward , backward , psq , th , accuracy , max_iters )     
     struct site *__restrict lat ; 
     GLU_complex *__restrict *__restrict gauge ;
     GLU_complex *__restrict *__restrict out ;
     GLU_complex *__restrict *__restrict in ; 
     const fftw_plan *forward ;
     const fftw_plan *backward ; 
     const GLU_real *__restrict psq ; 
     double *th ; 
     const double accuracy ;
     const int max_iters ;
{
  int i ;
  struct site *lat_copy = NULL , *lat_best = NULL ;

  GLU_malloc( (void**)&lat_copy , 16 , LVOLUME * sizeof( struct site ) ) ;
  init_navig( lat_copy ) ;
  GLU_malloc( (void**)&lat_best , 16 , LVOLUME * sizeof( struct site ) ) ;
  init_navig( lat_copy ) ;

#ifdef BEST_COPY
  double maxlink = 1.0 , newlink ;
#else
  double maxlink = 0.0 , newlink ;
#endif
  int copies , iters = 0 ;
  // loop over the number of gauge copies !
  for( copies = 0 ; copies < LUXURY_GAUGE ; copies++ ) {
    // copy our lattice fields
    #pragma omp parallel for private(i)
    PFOR(  i = 0 ; i < LVOLUME ; i++  ) {
      memcpy( &lat_copy[i] , &lat[i] , sizeof( struct site ) ) ;
    }
    // perform a random gauge transform
    random_transform( lat_copy , gauge ) ;
    // set the accuracy to be low
    const double acc = 1E-6 ;
    const double max = 1000 ;
    #ifdef GLU_GFIX_SD
    iters = FASD( lat_copy , gauge , 
		  out , in ,
		  forward , backward , 
		  psq , th , acc , max ) ; 
    #else
    iters = FACG( lat_copy , gauge , 
		  out , in ,
		  forward , backward , 
		  psq , th , acc , max ) ; 
    #endif
    
    // compute the link , wrap this to the functional?
    newlink = gauge_functional( lat_copy ) ;
    printf( "  [COPY] %d [FUNCTIONAL] %1.15f [ITER] %d " , 
	    copies , newlink , iters ) ; 
    #ifdef BEST_COPY 
    // the best copy is defined as the effective minimisation of the Gauge-functional 
    if( unlikely( newlink < maxlink ) && unlikely( iters != max ) ) 
    #else
    if( unlikely( newlink > maxlink ) && unlikely( iters != max ) ) 
    #endif
      {
	printf( " -> Copy accepted \n" ) ;
	maxlink = newlink ;
        #pragma omp parallel for private(i)
	PFOR(  i = 0 ; i < LVOLUME ; i++  ) {
	  memcpy( &lat_best[i] , &lat_copy[i] , sizeof( struct site ) ) ;
	}
      } else { printf( " -> Copy rejected %e\n" , *th ) ; }
  }
  // set our lattice to our chosen copy
  #pragma omp parallel for private(i)
  PFOR(  i = 0 ; i < LVOLUME ; i++  ) {
    memcpy( &lat[i] , &lat_best[i] , sizeof( struct site ) ) ;
  }
  // final convergence run god I hope this one doesn't fail! Pretty unlikely
  #ifdef GLU_GFIX_SD
  iters += FASD( lat , gauge , 
		 out , in , 
		 forward , backward , 
		 psq , 
		 th , accuracy , max_iters ) ; 
  #else
  iters += FACG( lat , gauge , 
		 out , in , 
		 forward , backward , 
		 psq , 
		 th , accuracy , max_iters ) ; 
  #endif

  // free the lattice temporaries
  free( lat_copy ) ;
  free( lat_best ) ;

  return iters ;
}

#endif // luxury gauge

#endif // FFTW

// should return failure if this really messes up
int
grab_file( struct site *__restrict lat , 
	   GLU_complex *__restrict *__restrict gauge , 
	   const char *__restrict infile )
{
  struct head_data HEAD_DATA ;
  FILE *config = fopen( infile , "rb" ) ;
  // assume the chksum is correct
  if( read_header( config , &HEAD_DATA , GLU_FALSE ) == GLU_FAILURE ||
      get_config_SUNC( config , lat , HEAD_DATA ) == GLU_FAILURE ) {
    printf( "[IO] binary file read error \n" ) ;
    return GLU_FAILURE ;
  } 
  random_transform( lat , gauge ) ; 
  fclose( config ) ;
  // could reperform checksum calculations here ....
  return GLU_SUCCESS ;
}

// Landau gauge fixing routine uses callbacks
int 
Landau( struct site *__restrict lat ,
	GLU_complex *__restrict *__restrict gauge ,
	const double accuracy ,
	const int iter ,
	const char *__restrict infile ,
	const GF_improvements improvement )
{
  double theta = 0. ; 

#ifdef HAVE_FFTW3_H
  if( parallel_ffts( ) == GLU_FAILURE ) {
    printf( "[PAR] Problem with initialising the OpenMP FFTW routines \n" ) ;
    // should clean up the memory here
    return GLU_FAILURE ;
  }

  fftw_plan *forward = malloc( ( TRUE_HERM ) * sizeof( fftw_plan ) ) ; 
  fftw_plan *backward = malloc( ( TRUE_HERM ) * sizeof( fftw_plan ) ) ; 
  GLU_complex **out = fftw_malloc( ( TRUE_HERM ) * sizeof( GLU_complex* ) ) ; 
  GLU_complex **in = fftw_malloc( ( TRUE_HERM ) * sizeof( GLU_complex* ) ) ; 

  int i ;
  #pragma omp parallel for private(i)
  PFOR(  i = 0 ; i < TRUE_HERM ; i++  ) {
    out[i] = ( GLU_complex* )fftw_malloc( LVOLUME * sizeof( GLU_complex ) ) ; 
    in[i] = ( GLU_complex* )fftw_malloc( LVOLUME * sizeof( GLU_complex ) ) ; 
  }

  GLU_real *psq = malloc( LVOLUME * sizeof( GLU_real ) ) ; 
  precompute_momenta( psq ) ;

  /////////////// Look for Wisdom //////////////

  create_plans_DFT( forward , backward , in , out , TRUE_HERM , ND ) ;

  ///////// End of the search for Wisdom //////
#else 
  GLU_complex **in = malloc( ( TRUE_HERM ) * sizeof( GLU_complex* ) ) ; 
  int i ;
  #pragma omp parallel for private(i)
  PFOR(  i = 0 ; i < TRUE_HERM ; i++  ) {
    GLU_malloc( (void**)&in[i] , 16 , LVOLUME * sizeof( GLU_complex ) ) ;
  }
  // these are really dummy variables that don't get used in the SD
  GLU_complex **out = NULL ;
  GLU_real *psq = NULL ;
  int *forward = NULL , *backward = NULL ;
#endif

  // set up the FA method callback
  select_callback( improvement )  ;

  int iters = FA_callback( lat , gauge , 
			   out , in ,
			   forward , backward , 
			   psq , 
			   &theta , accuracy , iter ) ;

  // random restart portion of the code
  if( unlikely( iters == GLU_FAILURE ) && ( improvement != SMPREC_IMPROVE ) ) {
    int failure = 0 ; 
    int iters2 = iters ; 
    for( failure = 1 ; failure < GF_GLU_FAILURES ; failure++ ) {
      //repeat procedure have to read in the file :( 
      if( grab_file( lat , gauge , infile ) == GLU_FAILURE ) { 
	printf( "[IO] Something funky happened when trying to read in config again!\n" ) ;
	goto MemFree ; 
      }
      if( improvement == MAG_IMPROVE ) { mag( lat , gauge ) ; }

      // and the callback
      iters = FA_callback( lat , gauge , 
			   out , in ,
			   forward , backward , 
			   psq , 
			   &theta , accuracy , iter ) ;

      // if we hit a failure, check
      if( iters != GLU_FAILURE ) {
	iters2 += iters ; 
	break ;
      } else {// print information about the last failure
	iters2 = failure * iter ;	
	print_failure_info( failure , iters2 ) ;
	printf( "\n[GF] Failure :: %d || Accuracy %1.4e\n" , 
		failure , theta ) ;
      }
    }
    // print out some information regarding the GF //
    iters = iters2 ;
  }
  // end of random transform loop -> Usually not needed //

 MemFree :
#ifdef HAVE_FFTW3_H
  // free mallocs
  #pragma omp parallel for private(i)
  PFOR( i = 0 ; i < TRUE_HERM ; i++ ) {
    fftw_destroy_plan( forward[i] ) ;   
    fftw_destroy_plan( backward[i] ) ;  
    fftw_free( out[i] ) ; 
    fftw_free( in[i] ) ; 
  }
  fftw_free( out ) ; 
  fftw_free( in ) ; 
  free( psq ) ; 
  free( forward ) ; 
  free( backward ) ; 
  fftw_cleanup( ) ; 
  #ifdef OMP_FFTW
  // parallel
  fftw_cleanup_threads( ) ;
  #endif
#else
  #pragma omp parallel for private(i)
  PFOR( i = 0 ; i < TRUE_HERM ; i++ ) {
    free( in[i] ) ; 
  }
  free( in ) ; 
#endif

  if( iters >= ( ( GF_GLU_FAILURES - 1 ) * iter ) ) {
    printf( "\n[GF] Failure to converge to a sufficient solution \n" 
	    "[GF] Please lower the GF_TUNE parameter from %g \n" , Latt.gf_alpha ) ;
    return GLU_FAILURE ;
  } else {
    output_fixing_info( lat , theta , iters ) ;
  }

  return iters ; 
}
