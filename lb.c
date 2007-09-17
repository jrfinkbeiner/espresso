/* $Id$
 *
 * This file is part of the ESPResSo distribution (http://www.espresso.mpg.de).
 * It is therefore subject to the ESPResSo license agreement which you
 * accepted upon receiving the distribution and by which you are
 * legally bound while utilizing this file in any form or way.
 * There is NO WARRANTY, not even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * You should have received a copy of that license along with this
 * program; if not, refer to http://www.espresso.mpg.de/license.html
 * where its current version can be found, or write to
 * Max-Planck-Institute for Polymer Research, Theory Group, 
 * PO Box 3148, 55021 Mainz, Germany. 
 * Copyright (c) 2002-2007; all rights reserved unless otherwise stated.
 */

/** \file lb.c
 *
 * Lattice Boltzmann algorithm for hydrodynamic degrees of freedom.
 *
 * Includes fluctuating LB and coupling to MD particles via frictional 
 * momentum transfer.
 *
 */

#include <mpi.h>
#include <tcl.h>
#include <stdio.h>
#include <fftw3.h>
#include "utils.h"
#include "parser.h"
#include "communication.h"
#include "grid.h"
#include "domain_decomposition.h"
#include "interaction_data.h"
#include "thermostat.h"
#include "lattice.h"
#include "halo.h"
#include "lb-d3q19.h"
#include "lb-boundaries.h"
#include "lb.h"

#ifdef LB

/** Flag indicating momentum exchange between particles and fluid */
int transfer_momentum = 0;

/** Struct holding the Lattice Boltzmann parameters */
LB_Parameters lbpar = { 0.0, 0.0, -1.0, -1.0, -1.0, 0.0, { 0.0, 0.0, 0.0} };

/** The DnQm model to be used. */
LB_Model lbmodel = { 19, d3q19_lattice, d3q19_coefficients, d3q19_w, NULL, 1./3. };
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * ! MAKE SURE THAT D3Q19 is #undefined WHEN USING OTHER MODELS !
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/* doesn't work yet */
#ifndef D3Q19
#error The implementation only works for D3Q19 so far!
#endif

/** The underlying lattice structure */
Lattice lblattice = { {0,0,0}, {0,0,0}, 0, 0, 0, 0, -1.0, -1.0, NULL, NULL };

/** Pointer to the fluid nodes
 * This variable is used for convenience instead of having to type lattice.fields everywhere */
LB_FluidNode *lbfluid=NULL;
LB_FluidNode *lbfluid_new=NULL;

/** Communicator for halo exchange between processors */
HaloCommunicator update_halo_comm[2] = { { 0, NULL }, { 0, NULL } };

/** The number of field variables on a local lattice site (counted in doubles). */
static int n_fields;

/** \name Derived parameters */
/*@{*/
/** Flag indicating whether fluctuations are present. */
static int fluct;

/** relaxation rate of shear modes */
static double gamma_shear = 0.0;
/** relaxation rate of bulk modes */
static double gamma_bulk = 0.0;
/** amplitudes of the fluctuations of the modes */
static double lb_phi[19];
/** amplitude of the fluctuations in the viscous coupling */
static double lb_coupl_pref = 0.0;
/*@}*/

/** The number of velocities of the LB model.
 * This variable is used for convenience instead of having to type lbmodel.n_veloc everywhere. */
static int n_veloc;

/** Lattice spacing.
 * This variable is used for convenience instead of having to type lbpar.agrid everywhere. */
static double agrid;

/** Lattice Boltzmann time step
 * This variable is used for convenience instead of having to type lbpar.tau everywhere. */
static double tau;

/** measures the MD time since the last fluid update */
static double fluidstep=0.0;

/** toggle flag indicating the current memory field for the populations */
static int toggle=0;

#ifdef ADDITIONAL_CHECKS
/** counts the random numbers drawn for fluctuating LB and the coupling */
static int rancounter=0;
/** counts the occurences of negative populations due to fluctuations */
static int failcounter=0;
#endif

/***********************************************************************/

#ifdef ADDITIONAL_CHECKS
static int compare_buffers(double *buf1, double *buf2, int size) {
  int ret;
  if (memcmp(buf1,buf2,size)) {
    char *errtxt;
    errtxt = runtime_error(128);
    ERROR_SPRINTF(errtxt,"{102 Halo buffers are not identical} ");
    ret = 1;
  } else {
    ret = 0;
  }
  return ret;
}

/** Checks consistency of the halo regions (ADDITIONAL_CHECKS)
 * This function can be used as an additional check. It test whether the 
 * halo regions have been exchanged correctly. */
static void lb_check_halo_regions() {

  int x,y,z, index, s_node, r_node, count=n_veloc;
  double *s_buffer, *r_buffer;
  MPI_Status status[2];

  r_buffer = malloc(count*sizeof(double));

  if (PERIODIC(0)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (y=0;y<lblattice.halo_grid[1];++y) {

	index  = get_linear_index(0,y,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[1];
	r_node = node_neighbors[0];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);
	}

	index = get_linear_index(lblattice.grid[0]+1,y,z,lblattice.halo_grid); 
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[0];
	r_node = node_neighbors[1];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);	  
	}

      }      
    }
  }

  if (PERIODIC(1)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,0,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[3];
	r_node = node_neighbors[2];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	}

      }
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,lblattice.grid[1]+1,z,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[2];
	r_node = node_neighbors[3];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	}

      }
    }
  }

  if (PERIODIC(2)) {
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,0,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[5];
	r_node = node_neighbors[4];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  if (compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d z=%d\n",2,index,x,y,lblattice.grid[2]);  
	}

      }
    }
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,lblattice.grid[2]+1,lblattice.halo_grid);
	s_buffer = lbfluid[index].n;
	s_node = node_neighbors[4];
	r_node = node_neighbors[5];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  compare_buffers(lbfluid[index].n,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  if(compare_buffers(lbfluid[index].n,s_buffer,count*sizeof(double)))
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d\n",2,index,x,y);
	}
      
      }
    }
  }

  free(r_buffer);

}
#endif /* ADDITIONAL_CHECKS */

#ifdef ADDITIONAL_CHECKS
MDINLINE void lb_lattice_sum() {
    int i,a,b,c,d,e,f;
    double (*w) = lbmodel.w;
    double (*v)[3] = lbmodel.c;
    double sum;
    int count=0;
    
    for (a=0; a<3; a++)
	for (b=0; b<3; b++)
	    for (c=0; c<3; c++)
		for (d=0; d<3; d++)
		    for (e=0; e<3; e++)
			for (f=0; f<3; f++) {
			    sum = 0.0;
			    for (i=0; i<n_veloc; ++i) {
				sum += w[i]*v[i][a]*v[i][b]*v[i][c]*v[i][d]*v[i][e]*v[i][f];
			    }
			    if (sum!=0.0) { count++; fprintf(stderr,"(%d,%d,%d,%d,%d,%d) %f\n",a,b,c,d,e,f,sum/SQR(lbmodel.c_sound_sq)); }
			}
    fprintf(stderr,"%d non-null entries\n",count);

}
#endif

#ifdef ADDITIONAL_CHECKS
MDINLINE void lb_check_mode_transformation(LB_FluidNode *node) {

  /* check if what I think is right */

  int i;
  double *n = node->n;
  double *w = lbmodel.w;
  double (*e)[19] = d3q19_modebase;
  double sum_n=0.0, sum_m=0.0;
  double *mode = node->modes;
  double n_eq[19];
  double m_eq[19];
  double avg_rho = lbpar.rho;
  double (*c)[3] = lbmodel.c;

  m_eq[0] = mode[0];
  m_eq[1] = mode[1];
  m_eq[2] = mode[2];
  m_eq[3] = mode[3];

  double rho = mode[0] + avg_rho;
  double *j  = mode+1;

  /* equilibrium part of the stress modes */
  /* remember that the modes have (\todo not?) been normalized! */
  m_eq[4] = /*1./6.*/scalar(j,j)/rho;
  m_eq[5] = /*1./4.*/(SQR(j[0])-SQR(j[1]))/rho;
  m_eq[6] = /*1./12.*/(scalar(j,j) - 3.0*SQR(j[2]))/rho;
  m_eq[7] = j[0]*j[1]/rho;
  m_eq[8] = j[0]*j[2]/rho;
  m_eq[9] = j[1]*j[2]/rho;

  for (i=10;i<n_veloc;i++) {
    m_eq[i] = 0.0;
  }

  for (i=0;i<n_veloc;i++) {
    n_eq[i] = w[i]*((rho-avg_rho) + 3.*scalar(j,c[i]) + 9./2.*SQR(scalar(j,c[i]))/rho - 3./2.*scalar(j,j)/rho);
  } 

  for (i=0;i<n_veloc;i++) {
    sum_n += SQR(n[i]-n_eq[i])/w[i];
    sum_m += SQR(mode[i]-m_eq[i])/e[19][i];
  }

  if (fabs(sum_n-sum_m)>ROUND_ERROR_PREC) {    
    fprintf(stderr,"Attention: sum_n=%f sum_m=%f %e\n",sum_n,sum_m,fabs(sum_n-sum_m));
  }

}

MDINLINE void lb_init_mode_transformation() {

#ifdef D3Q19
  int i, j, k, l;
  double *w     = lbmodel.w;
  double (*c)[3]= lbmodel.c;
  double b[19][19];
  double e[19][19];
  double proj, norm[19];

  /* construct polynomials from the discrete velocity vectors */
  for (i=0;i<n_veloc;i++) {
    b[0][i]  = 1;
    b[1][i]  = c[i][0];
    b[2][i]  = c[i][1];
    b[3][i]  = c[i][2];
    b[4][i]  = scalar(c[i],c[i]);
    b[5][i]  = c[i][0]*c[i][0]-c[i][1]*c[i][1];
    b[6][i]  = scalar(c[i],c[i])-3*c[i][2]*c[i][2];
    //b[5][i]  = 3*c[i][0]*c[i][0]-scalar(c[i],c[i]);
    //b[6][i]  = c[i][1]*c[i][1]-c[i][2]*c[i][2];
    b[7][i]  = c[i][0]*c[i][1];
    b[8][i]  = c[i][0]*c[i][2];
    b[9][i]  = c[i][1]*c[i][2];
    b[10][i] = 3*scalar(c[i],c[i])*c[i][0];
    b[11][i] = 3*scalar(c[i],c[i])*c[i][1];
    b[12][i] = 3*scalar(c[i],c[i])*c[i][2];
    b[13][i] = (c[i][1]*c[i][1]-c[i][2]*c[i][2])*c[i][0];
    b[14][i] = (c[i][0]*c[i][0]-c[i][2]*c[i][2])*c[i][1];
    b[15][i] = (c[i][0]*c[i][0]-c[i][1]*c[i][1])*c[i][2];
    b[16][i] = 3*scalar(c[i],c[i])*scalar(c[i],c[i]);
    b[17][i] = 2*scalar(c[i],c[i])*b[5][i];
    b[18][i] = 2*scalar(c[i],c[i])*b[6][i];
  }

  /* Gram-Schmidt orthogonalization procedure */
  for (j=0;j<n_veloc;j++) {
    for (i=0;i<n_veloc;i++) e[j][i] = b[j][i];
    for (k=0;k<j;k++) {
      proj = 0.0;
      for (l=0;l<n_veloc;l++) {
	proj += w[l]*e[k][l]*b[j][l];
      }
      for (i=0;i<n_veloc;i++) e[j][i] -= proj/norm[k]*e[k][i];
    }
    norm[j] = 0.0;
    for (i=0;i<n_veloc;i++) norm[j] += w[i]*SQR(e[j][i]);
  }
  
  fprintf(stderr,"e[%d][%d] = {\n",n_veloc,n_veloc);
  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"{ % .1f",e[i][0]);
    for (j=1;j<n_veloc;j++) {
      fprintf(stderr,", % .1f",e[i][j]);
    }
    fprintf(stderr," } %.2f\n",norm[i]);
  }
  fprintf(stderr,"};\n");

#else
  int i, j, k, l;
  double b[9][9];
  double e[9][9];
  double proj, norm[9];

  double c[9][2] = { { 0, 0 },
		     { 1, 0 },
		     {-1, 0 },
                     { 0, 1 },
                     { 0,-1 },
		     { 1, 1 },
		     {-1,-1 },
		     { 1,-1 },
		     {-1, 1 } };

  double w[9] = { 4./9, 1./9, 1./9, 1./9, 1./9, 1./36, 1./36, 1./36, 1./36 };

  n_veloc = 9;

  /* construct polynomials from the discrete velocity vectors */
  for (i=0;i<n_veloc;i++) {
    b[0][i] = 1;
    b[1][i] = c[i][0];
    b[2][i] = c[i][1];
    b[3][i] = 3*(SQR(c[i][0]) + SQR(c[i][1]));
    b[4][i] = c[i][0]*c[i][0]-c[i][1]*c[i][1];
    b[5][i] = c[i][0]*c[i][1];
    b[6][i] = 3*(SQR(c[i][0])+SQR(c[i][1]))*c[i][0];
    b[7][i] = 3*(SQR(c[i][0])+SQR(c[i][1]))*c[i][1];
    b[8][i] = (b[3][i]-5)*b[3][i]/2;
  }

  /* Gram-Schmidt orthogonalization procedure */
  for (j=0;j<n_veloc;j++) {
    for (i=0;i<n_veloc;i++) e[j][i] = b[j][i];
    for (k=0;k<j;k++) {
      proj = 0.0;
      for (l=0;l<n_veloc;l++) {
	proj += w[l]*e[k][l]*b[j][l];
      }
      for (i=0;i<n_veloc;i++) e[j][i] -= proj/norm[k]*e[k][i];
    }
    norm[j] = 0.0;
    for (i=0;i<n_veloc;i++) norm[j] += w[i]*SQR(e[j][i]);
  }
  
  fprintf(stderr,"e[%d][%d] = {\n",n_veloc,n_veloc);
  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"{ % .1f",e[i][0]);
    for (j=1;j<n_veloc;j++) {
      fprintf(stderr,", % .1f",e[i][j]);
    }
    fprintf(stderr," } %.2f\n",norm[i]);
  }
  fprintf(stderr,"};\n");

#endif

}
#endif /* ADDITIONAL_CHECKS */

#ifdef ADDITIONAL_CHECKS
/** Check for negative populations.  
 *
 * Checks for negative populations and increases failcounter for each
 * occurence.
 *
 * @param  local_node Pointer to the local lattice site (Input).
 * @return Number of negative populations on the local lattice site.
 */
MDINLINE int lb_check_negative_n(LB_FluidNode *local_node) {
  int i, localfails=0;
  const double *local_n = local_node->n;

  for (i=0; i<n_veloc; i++) {
    if (local_n[i]+lbmodel.coeff[i][0]*lbpar.rho < 0.0) {
      ++localfails;
      ++failcounter;
      fprintf(stderr,"%d: Negative population n[%d]=%le (failcounter=%d, rancounter=%d).\n   Check your parameters if this occurs too often!\n",this_node,i,lbmodel.coeff[i][0]*lbpar.rho+local_n[i],failcounter,rancounter);
      break;
   }
  }

  return localfails;
}
#endif /* ADDITIONAL_CHECKS */

/***********************************************************************/

/** Performs basic sanity checks. */
static int lb_sanity_checks() {

  char *errtxt;
  int ret = 0;

    if (cell_structure.type != CELL_STRUCTURE_DOMDEC) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{103 LB requires domain-decomposition cellsystem} ");
      ret = -1;
    } 
    else if (dd.use_vList) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{104 LB requires no Verlet Lists} ");
      ret = -1;
    }    

    return ret;

}

/***********************************************************************/

/** (Re-)allocate memory for the fluid and initialize pointers. */
static void lb_create_fluid() {

  int index;

  lblattice.fields = realloc(lblattice.fields,2*lblattice.halo_grid_volume*sizeof(LB_FluidNode));
  lblattice.data = realloc(lblattice.data,2*lblattice.halo_grid_volume*n_fields*sizeof(double));

  lbfluid = (LB_FluidNode *)lblattice.fields;
  lbfluid_new = &lbfluid[lblattice.halo_grid_volume];
  lbfluid[0].n = (double *)lblattice.data;
#ifndef D3Q19
  lbfluid[0].n_tmp = (double *)lblattice.data + lblattice.halo_grid_volume*n_veloc;
#endif
  
  for (index=0; index<2*lblattice.halo_grid_volume; index++) {
    lbfluid[index].n = lbfluid[0].n + index*n_veloc;
#ifndef D3Q19
    lbfluid[index].n_tmp = lbfluid[0].n_tmp + index*n_veloc;
#endif
  }

  int lens[2] = { n_veloc, 1 };
  MPI_Aint disps[2] = { 0, n_veloc*sizeof(double) };
  MPI_Datatype types[2] = { MPI_DOUBLE, MPI_UB };
  //KG: Quick fix
  //MPI_Type_free(&lblattice.datatype);
  MPI_Type_struct(2, lens, disps, types, &lblattice.datatype);
  MPI_Type_commit(&lblattice.datatype);
  LB_TRACE(fprintf(stderr,"Potential memory hole!\n"));

}

/** Sets up the structures for exchange of the halo regions.
 *  See also \ref halo.c */
static void lb_prepare_communication() {

    /* create types for lattice data layout */
    int lens[2] = { n_veloc*sizeof(double), 1 };
    int disps[2] = { 0, n_veloc*sizeof(double) };
    Fieldtype fieldtype;
    halo_create_fieldtype(1, lens, disps, disps[1], &fieldtype);

    /* setup the halo communication */
    prepare_halo_communication(&update_halo_comm[0],&lblattice,fieldtype,lblattice.datatype,0);
    prepare_halo_communication(&update_halo_comm[1],&lblattice,fieldtype,lblattice.datatype,lblattice.halo_grid_volume*n_fields*sizeof(double));
 
    halo_free_fieldtype(&fieldtype);

}

/** Release the fluid. */
static void lb_release_fluid() {
  MPI_Type_free(&lblattice.datatype);
  free(lbfluid[0].n);
  free(lbfluid);
}

/** (Re-)initializes the fluid. */
void lb_reinit_parameters() {
  int i;

  agrid   = lbpar.agrid;
  tau     = lbpar.tau;

  n_veloc = lbmodel.n_veloc;

  /* number of double entries in the data fields */
  n_fields = n_veloc;
#ifndef D3Q19
  n_fields += n_veloc; /* temporary velocity populations */
#endif

  /* Eq. (80) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
  gamma_shear = 1. - 2./(6.*lbpar.viscosity*tau/(agrid*agrid)+1.);

  if (lbpar.bulk_viscosity > 0.0) {
    /* Eq. (81) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
    gamma_bulk = 1. - 2./(9.*lbpar.bulk_viscosity*tau/(agrid*agrid)+1.);
  }

  if (temperature > 0.0) {  /* fluctuating hydrodynamics ? */

    fluct = 1;

    /* Eq. (51) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007).
     * Note that the modes are not normalized as in the paper here! */
    double mu = temperature/lbmodel.c_sound_sq*tau*tau/(agrid*agrid);
    double (*e)[19] = d3q19_modebase;
    for (i=0; i<3; i++) lb_phi[i] = 0.0;
    lb_phi[4] = sqrt(mu*e[19][4]*(1.-SQR(gamma_bulk)));
    for (i=5; i<10; i++) lb_phi[i] = sqrt(mu*e[19][i]*(1.-SQR(gamma_shear)));
    for (i=10; i<n_veloc; i++) lb_phi[i] = sqrt(mu*e[19][i]);
 
    LB_TRACE(fprintf(stderr,"%d: gamma_shear=%f gamma_bulk=%f shear_fluct=%f bulk_fluct=%f mu=%f\n",this_node,gamma_shear,gamma_bulk,lb_phi[9],lb_phi[4],mu));

    /* lb_coupl_pref is stored in MD units (force)
     * Eq. (16) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
     * The factor 12 comes from the fact that we use random numbers
     * from -0.5 to 0.5 (equally distributed) which have variance 1/12.
     * time_step comes from the discretization.
     */
    lb_coupl_pref = sqrt(12.*2.*lbpar.friction*temperature/time_step); 

    LB_TRACE(fprintf(stderr,"%d: lb_coupl_pref=%f (temp=%f, friction=%f, time_step=%f)\n",this_node,lb_coupl_pref,temperature,lbpar.friction,time_step));

  } else {
    /* no fluctuations at zero temperature */
    fluct = 0;
    for (i=0;i<n_veloc;i++) lb_phi[i] = 0.0;
  }

}

/** (Re-)initializes the fluid according to the given value of rho. */
void lb_reinit_fluid() {

    int k ;

    /* default values for fields in lattice units */
    double rho = lbpar.rho/(agrid*agrid*agrid) ;
    double v[3] = { 0., 0., 0. };
    double pi[6] = { rho*lbmodel.c_sound_sq, 0., rho*lbmodel.c_sound_sq, 0., 0., rho*lbmodel.c_sound_sq };

    for (k=0;k<lblattice.halo_grid_volume;k++) {

#ifdef CONSTRAINTS
      if (lbfluid[k].boundary==0) {
	lb_set_local_fields(&lbfluid[k],rho,v,pi);
      } else {
        lb_set_boundary_fields(&lbfluid[k],rho,v,pi);
      }
#else
      lb_set_local_fields(&lbfluid[k],rho,v,pi);
#endif

    }

}

/** Performs a full initialization of
 *  the Lattice Boltzmann system. All derived parameters
 *  and the fluid are reset to their default values. */
void lb_init() {

  if (lb_sanity_checks()) return;

  /* initialize derived parameters */
  lb_reinit_parameters();

  /* initialize the local lattice domain */
  init_lattice(&lblattice,agrid,tau);  

  if (check_runtime_errors()) return;

  /* allocate memory for data structures */
  lb_create_fluid();

#ifdef CONSTRAINTS
  /* setup boundaries of constraints */
  lb_init_constraints();
#endif

  /* setup the initial particle velocity distribution */
  lb_reinit_fluid();

  /* prepare the halo communication */
  lb_prepare_communication();

}

/** Release fluid and communication. */
void lb_release() {
  release_halo_communication(&update_halo_comm[0]);
  release_halo_communication(&update_halo_comm[1]);
  lb_release_fluid();
}

/***********************************************************************/
/** \name Mapping between hydrodynamic fields and particle populations */
/***********************************************************************/
/*@{*/

/** Calculate local populations from hydrodynamic fields.
 *
 * The mapping is given in terms of the equilibrium distribution.
 *
 * Eq. (2.15) Ladd, J. Fluid Mech. 271, 295-309 (1994)
 * Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005)
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param trace      Trace of the local stress tensor (Input).
 * @param trace_eq   Trace of equilibriumd part of local stress tensor (Input).
 */
MDINLINE void lb_calc_n_equilibrium(LB_FluidNode *local_node) {

  double *local_n   = local_node->n;
  double *local_rho = local_node->rho;
  double *local_j   = local_node->j;
  double *local_pi  = local_node->pi;
  double trace;
  const double rhoc_sq = *local_rho*lbmodel.c_sound_sq;
  const double avg_rho = lbpar.rho/(agrid*agrid*agrid);

  /* see Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005) */
  
  /* reduce the pressure tensor to the part needed here */
  local_pi[0] -= rhoc_sq;
  local_pi[2] -= rhoc_sq;
  local_pi[5] -= rhoc_sq;

  trace = local_pi[0] + local_pi[2] + local_pi[5];

#ifdef D3Q19
  double rho_times_coeff;
  double tmp1,tmp2;

  /* update the q=0 sublattice */
  local_n[0] = 1./3. * (*local_rho-avg_rho) - 1./2.*trace;

  /* update the q=1 sublattice */
  rho_times_coeff = 1./18. * (*local_rho-avg_rho);

  local_n[1] = rho_times_coeff + 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  local_n[2] = rho_times_coeff - 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  local_n[3] = rho_times_coeff + 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  local_n[4] = rho_times_coeff - 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  local_n[5] = rho_times_coeff + 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;
  local_n[6] = rho_times_coeff - 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;

  /* update the q=2 sublattice */
  rho_times_coeff = 1./36. * (*local_rho-avg_rho);

  tmp1 = local_pi[0] + local_pi[2];
  tmp2 = 2.0*local_pi[1];

  local_n[7]  = rho_times_coeff + 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[8]  = rho_times_coeff - 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[9]  = rho_times_coeff + 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[10] = rho_times_coeff - 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[0] + local_pi[5];
  tmp2 = 2.0*local_pi[3];

  local_n[11] = rho_times_coeff + 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[12] = rho_times_coeff - 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[13] = rho_times_coeff + 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[14] = rho_times_coeff - 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[2] + local_pi[5];
  tmp2 = 2.0*local_pi[4];

  local_n[15] = rho_times_coeff + 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[16] = rho_times_coeff - 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  local_n[17] = rho_times_coeff + 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  local_n[18] = rho_times_coeff - 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

#else
  int i;
  double tmp=0.0;
  double (*c)[3] = lbmodel.c;
  double (*coeff)[4] = lbmodel.coeff;

  for (i=0;i<n_veloc;i++) {

    tmp = local_pi[0]*c[i][0]*c[i][0]
      + (2.0*local_pi[1]*c[i][0]+local_pi[2]*c[i][1])*c[i][1]
      + (2.0*(local_pi[3]*c[i][0]+local_pi[4]*c[i][1])+local_pi[5]*c[i][2])*c[i][2];

    local_n[i] =  coeff[i][0] * (*local_rho-avg_rho);
    local_n[i] += coeff[i][1] * scalar(local_j,c[i]);
    local_n[i] += coeff[i][2] * tmp;
    local_n[i] += coeff[i][3] * trace;

  }
#endif

  /* restore the pressure tensor to the full part */
  local_pi[0] += rhoc_sq;
  local_pi[2] += rhoc_sq;
  local_pi[5] += rhoc_sq;

}
  
#if 0
MDINLINE void lb_map_fields_to_populations() {
    int k;

    for (k=0; k<lblattice.halo_grid_volume; k++) {
	lb_calc_local_fields(&lbfluid[k],1);
    }

}

MDINLINE void lb_map_populations_to_fields() {
    int k;

    for (k=0; k<lblattice.halo_grid_volume; k++) {
	lb_calc_local_n(&lbfluid[k]);
    }

}
#endif

/*@}*/

/***********************************************************************/
/** \name Collision step */
/***********************************************************************/
/*@{*/

/** Collision update of the stress tensor.
 * The stress tensor is relaxed towards the equilibrium.
 *
 * See Eq. (5) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005)
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param trace      Trace of local stress tensor (Output).
 * @param trace_eq   Trace of equilibrium part of local stress tensor (Output).
 */
#if 0
MDINLINE void lb_update_local_pi(LB_FluidNode *local_node) {

  const double local_rho = *(local_node->rho);
  double *local_j  = local_node->j;
  double *local_pi = local_node->pi;
  double local_pi_eq[6];
  double trace, trace_eq;
  double tmp;

  const double rhoc_sq = local_rho*lbmodel.c_sound_sq;
  const double onepluslambda = 1.0 + lblambda;

  /* calculate the equilibrium part of the pressure tensor */
  local_pi_eq[0] = rhoc_sq + local_j[0]*local_j[0]/local_rho;
  tmp = local_j[1]/local_rho;
  local_pi_eq[1] = local_j[0]*tmp;
  local_pi_eq[2] = rhoc_sq + local_j[1]*tmp;
  tmp = local_j[2]/local_rho;
  local_pi_eq[3] = local_j[0]*tmp;
  local_pi_eq[4] = local_j[1]*tmp;
  local_pi_eq[5] = rhoc_sq + local_j[2]*tmp;

  /* calculate the traces */
  trace_eq = local_pi_eq[0] + local_pi_eq[2] + local_pi_eq[5];
  trace = local_pi[0] + local_pi[2] + local_pi[5];
    
  /* relax the local pressure tensor */
  local_pi[0] = local_pi_eq[0] + onepluslambda*(local_pi[0] - local_pi_eq[0]);
  local_pi[1] = local_pi_eq[1] + onepluslambda*(local_pi[1] - local_pi_eq[1]);
  local_pi[2] = local_pi_eq[2] + onepluslambda*(local_pi[2] - local_pi_eq[2]);
  local_pi[3] = local_pi_eq[3] + onepluslambda*(local_pi[3] - local_pi_eq[3]);
  local_pi[4] = local_pi_eq[4] + onepluslambda*(local_pi[4] - local_pi_eq[4]);
  local_pi[5] = local_pi_eq[5] + onepluslambda*(local_pi[5] - local_pi_eq[5]);  
  tmp = 1./3.*(lblambda_bulk-lblambda)*(trace - trace_eq);
  local_pi[0] += tmp;
  local_pi[2] += tmp;
  local_pi[5] += tmp;

}
#endif

/** Add fluctuating part to the stress tensor and update the populations.
 *
 * Ladd, J. Fluid Mech. 271, 285-309 (1994).<br>
 * Berk Usta, Ladd and Butler, JCP 122, 094902 (2005).<br>
 * Ahlrichs, PhD-Thesis (2000).
 *   
 * @param local_node Pointer to the local lattice site.
 */
#if 0
MDINLINE void lb_add_fluct_pi(LB_FluidNode *local_node) {

  double *local_pi = local_node->pi;
  double tmp, sum=0.0;

  const double pref1 = sqrt(2) * lb_fluct_pref;

  /* off-diagonal components */
  local_pi[1] += lb_fluct_pref * (d_random()-0.5);
  local_pi[3] += lb_fluct_pref * (d_random()-0.5);
  local_pi[4] += lb_fluct_pref * (d_random()-0.5);

  /* diagonal components */
  tmp = (d_random()-0.5);
  sum += tmp;
  local_pi[0] += pref1 * tmp;
  tmp = (d_random()-0.5);
  sum += tmp;
  local_pi[2] += pref1 * tmp;
  tmp = (d_random()-0.5);
  sum += tmp;
  local_pi[5] += pref1 * tmp;

  /* make shear modes traceless and add bulk fluctuations on the trace */
  sum *= (lb_fluct_pref_bulk/sqrt(3) - pref1/3.0);
  local_pi[0] += sum;
  local_pi[2] += sum;
  local_pi[5] += sum;

#ifdef ADDITIONAL_CHECKS
  rancounter += 6;
#endif

}
#endif

#if 0
MDINLINE void lb_calc_modes(LB_FluidNode *node) {
  int i;
  double *n    = node->n;
  double *mode = node->modes;
  //double (*e)[n_veloc] = d3q19_modebase;
  //double avg_rho = lbpar.rho*agrid*agrid*agrid;

#ifdef D3Q19
  /* mass mode */
  mode[ 0] =   n[ 0] + n[ 1] + n[ 2] + n[ 3] + n[4] + n[5] + n[6]
             + n[ 7] + n[ 8] + n[ 9] + n[10]
             + n[11] + n[12] + n[13] + n[14]
             + n[15] + n[16] + n[17] + n[18];
  /* momentum modes */
  mode[ 1] =   n[ 1] - n[ 2] 
             + n[ 7] - n[ 8] + n[ 9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[ 2] =   n[ 3] - n[ 4]
             + n[ 7] - n[ 8] - n[ 9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[ 3] =   n[ 5] - n[ 6]
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];
  /* stress modes */
  mode[ 4] = - n[ 0] 
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18];
  mode[ 5] =   n[ 1] + n[ 2] - n[ 3] - n[4]
             + n[11] + n[12] + n[13] + n[14] - n[15] - n[16] - n[17] - n[18];
  mode[ 6] =   n[ 1] + n[ 2] + n[ 3] + n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             - 2.*(n[5] + n[6] - n[7] - n[8] - n[9] - n[10]);
  mode[ 7] =   n[ 7] + n[ 8] - n[ 9] - n[10];
  mode[ 8] =   n[11] + n[12] - n[13] - n[14];
  mode[ 9] =   n[15] + n[16] - n[17] - n[18];

  /* ghost modes (no equilibrium part due to orthogonality) */
  mode[10] = 2.*(n[2] - n[1]) 
             + n[7] - n[8] + n[9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[11] = 2.*(n[4] - n[3])
             + n[7] - n[8] - n[9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[12] = 2.*(n[6] - n[5])
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];
  mode[13] =   n[ 7] - n[ 8] + n[ 9] - n[10] - n[11] + n[12] - n[13] + n[14];
  mode[14] =   n[ 7] - n[ 8] - n[ 9] + n[10] - n[15] + n[16] - n[17] + n[18];
  mode[15] =   n[11] - n[12] - n[13] + n[14] - n[15] + n[16] + n[17] - n[18];
  mode[16] =   n[ 0]
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18]
             - 2.*(n[1] + n[2] + n[3] + n[4] + n[5] + n[6]);
  mode[17] =   n[ 3] + n[ 4] - n[ 1] - n[ 2] 
             + n[11] + n[12] + n[13] + n[14] 
             - n[15] - n[16] - n[17] - n[18];
  mode[18] = - n[ 1] - n[ 2] - n[ 3] - n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             + 2.*(n[5] + n[6] + n[7] + n[8] + n[9] + n[10]);

  /* normalize the modes */
  for (i=0;i<n_veloc;i++) {
    //mode[i] /= e[19][i];
  }

  /* check if what I think is right... */
  //int j, k;
  //double *w = lbmodel.w;
  //double sum=0.0, sum2=0.0;
  //for (i=0;i<n_veloc;i++) {
  //  for (j=0;j<n_veloc;j++) {
  //    sum =0.0; sum2=0.0;
  //    for (k=0;k<n_veloc;k++) {
  //	sum += w[i]/e[19][k]*e[k][i]*e[k][j];
  //	sum2 += w[k]/e[19][i]*e[i][k]*e[j][k];
  //    }
  //    fprintf(stderr,"(%d,%d)=%f %f\n",i,j,sum,sum2);
  //  }
  //}

#else
  int k;
  double (*e)[n_veloc] = d3q19_modebase;
  double mode2[n_veloc];
  for (i=0;i<n_veloc;i++) {
    mode2[i] = 0.0;
    for (k=0;k<n_veloc;k++) {
      mode2[i] += e[i][k]*(n[k]);
    }
    fprintf(stderr,"mode[%d]=%f mode2[%d]=%f\n",i,mode[i],i,mode2[i]);
  }
#endif

}
#endif

#if 0
/** The Lattice Boltzmann collision step.
 * Loop over all lattice sites and perform the collision update.
 * If fluctuations are present, the fluctuating part of the stress tensor
 * is added. The update is only accepted then, if no negative populations
 * occur.
 */
MDINLINE void lb_calc_collisions() {

  int index, x, y, z;
  LB_FluidNode *local_node;
  
  /* loop over all nodes (halo excluded) */
  index = lblattice.halo_offset;
  for (z=1;z<=lblattice.grid[2];z++) {
    for (y=1;y<=lblattice.grid[1];y++) {
      for (x=1;x<=lblattice.grid[0];x++) {

	local_node = &lbfluid[index];

#ifdef CONSTRAINTS
	if (local_node->boundary==0)
#endif
	{

	  lb_calc_modes(local_node);
	  
	  //lb_check_mode_transformation(local_node);

#ifdef ADDITIONAL_CHECKS
	  double old_rho = local_node->modes[0];
#endif

	  lb_relax_modes(local_node);
	  
	  if (fluct) lb_thermalize_modes(local_node);
	  
	  lb_calc_n_from_modes(local_node);

#ifdef ADDITIONAL_CHECKS
	  if (local_node->modes[0] < -lbpar.rho) {
	    char *errtxt = runtime_error(128 + TCL_DOUBLE_SPACE + 3*TCL_INTEGER_SPACE);
	    ERROR_SPRINTF(errtxt,"{107 Negative density %le in lb_calc_collisions on site (%d,%d,%d)} ",local_node->modes[0],x,y,z);
	  }
#endif
	  
#ifdef ADDITIONAL_CHECKS
	  double *local_rho = local_node->rho;
	  lb_calc_local_rho(local_node);
	  if (fabs(*local_rho-lbpar.rho-old_rho) > ROUND_ERROR_PREC) {
	    char *errtxt = runtime_error(128 + TCL_DOUBLE_SPACE + 3*TCL_INTEGER_SPACE);
	    ERROR_SPRINTF(errtxt,"{106 Mass loss/gain %le in lb_calc_collisions on site (%d,%d,%d)} ",*local_rho-lbpar.rho-old_rho,x,y,z);
	  }
#endif

	}

	++index;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

}
#endif

/*@}*/

/***********************************************************************/
/** \name Streaming step */
/***********************************************************************/
/*@{*/

/** The Lattice Boltzmann streaming step.
 * The populations are moved to the neighbouring lattice sites
 * according to the velocity sublattice. This can be done in two ways:
 * First, one can use a temporary field to store the updated configuration.
 * Second, one can order the updates such that only populations are
 * overwritten which have already been propagated. The halo region
 * serves as a buffer. This requires two sweeps through the lattice,
 * one bottom up and one bottom down. One has to be careful if the
 * velocities are upgoing or downgoing. This can be a real bugfest!
 */
#if 0
MDINLINE void lb_propagate_n() {

  int yperiod = lblattice.halo_grid[0];
  int zperiod = lblattice.halo_grid[0]*lblattice.halo_grid[1];
  int next[n_veloc];
  int k, index;

#ifdef D3Q19

  next[0]  =   n_veloc * 0 + 0;                  // ( 0, 0, 0) =
  next[1]  =   n_veloc * 1 + 1;                  // ( 1, 0, 0) +
  next[2]  = - n_veloc * 1 + 2;                  // (-1, 0, 0)
  next[3]  =   n_veloc * yperiod + 3;            // ( 0, 1, 0) +
  next[4]  = - n_veloc * yperiod + 4;            // ( 0,-1, 0)
  next[5]  =   n_veloc * zperiod + 5;            // ( 0, 0, 1) +
  next[6]  = - n_veloc * zperiod + 6;            // ( 0, 0,-1)
  next[7]  =   n_veloc * (1+yperiod) + 7;        // ( 1, 1, 0) +
  next[8]  = - n_veloc * (1+yperiod) + 8;        // (-1,-1, 0)
  next[9]  =   n_veloc * (1-yperiod) + 9;        // ( 1,-1, 0)
  next[10] = - n_veloc * (1-yperiod) + 10;       // (-1, 1, 0) +
  next[11] =   n_veloc * (1+zperiod) + 11;       // ( 1, 0, 1) +
  next[12] = - n_veloc * (1+zperiod) + 12;       // (-1, 0,-1)
  next[13] =   n_veloc * (1-zperiod) + 13;       // ( 1, 0,-1)
  next[14] = - n_veloc * (1-zperiod) + 14;       // (-1, 0, 1) +
  next[15] =   n_veloc * (yperiod+zperiod) + 15; // ( 0, 1, 1) +
  next[16] = - n_veloc * (yperiod+zperiod) + 16; // ( 0,-1,-1)
  next[17] =   n_veloc * (yperiod-zperiod) + 17; // ( 0, 1,-1)
  next[18] = - n_veloc * (yperiod-zperiod) + 18; // ( 0,-1, 1) +

  double *n = lbfluid[0].n;

  //fprintf(stderr,"local_n[7]=%e\n",lbfluid[get_linear_index(0,0,21,lblattice.halo_grid)].n[7]+lbmodel.coeff[7][0]*lbpar.rho);

  //index = get_linear_index(1,1,0,lblattice.halo_grid);
  //n = lbfluid[index].n;
  //int i;
  //for (i=0;i<n_veloc;++i) {
  //  fprintf(stderr,"n[%d]=%e\n",i,n[i]);
  //}
  //lb_calc_local_rho(&lbfluid[index]);
  //fprintf(stderr,"rho=%e\n",*(lbfluid[index].rho));

  n = lbfluid[0].n;

  /* top down sweep */
  index = (lblattice.halo_grid_volume-lblattice.halo_offset-1)*n_veloc;
  for (k=lblattice.halo_grid_volume-lblattice.halo_offset-1;k>=0;k--) {
      
    /* propagation to higher indices */
    n[index+next[1]]  = n[index+1];
    n[index+next[3]]  = n[index+3];
    n[index+next[5]]  = n[index+5];
    n[index+next[7]]  = n[index+7];
    //if (&n[index+next[7]-7]==lbfluid[get_linear_index(0,0,21,lblattice.halo_grid)].n) {
    //  int x,y,z;
    //  get_grid_pos(k,&x,&y,&z,lblattice.halo_grid);
    //  fprintf(stderr,"index=%d (%d,%d,%d)\n",index,x,y,z);
    //}
    n[index+next[10]] = n[index+10];
    n[index+next[11]] = n[index+11];
    n[index+next[14]] = n[index+14];
    n[index+next[15]] = n[index+15];
    n[index+next[18]] = n[index+18];

    index -= n_veloc;
  }

  //fprintf(stderr,"local_n[7]=%e\n",lbfluid[get_linear_index(0,0,21,lblattice.halo_grid)].n[7]+lbmodel.coeff[7][0]*lbpar.rho);

  /* bottom up sweep */
  index = lblattice.halo_offset*n_veloc;
  for (k=lblattice.halo_offset;k<lblattice.halo_grid_volume;k++) {

    /* propagation to lower indices */
    n[index+next[2]]  = n[index+2];
    n[index+next[4]]  = n[index+4];
    n[index+next[6]]  = n[index+6];
    n[index+next[8]]  = n[index+8];
    n[index+next[9]]  = n[index+9];
    n[index+next[12]] = n[index+12];
    n[index+next[13]] = n[index+13];
    n[index+next[16]] = n[index+16];
    n[index+next[17]] = n[index+17];

    index += n_veloc;
  }

  //fprintf(stderr,"local_n[7]=%e\n",lbfluid[get_linear_index(0,0,21,lblattice.halo_grid)].n[7]+lbmodel.coeff[7][0]*lbpar.rho);

  //index = get_linear_index(1,1,1,lblattice.halo_grid);
  //n = lbfluid[index].n;
  //for (i=0;i<n_veloc;++i) {
  //  fprintf(stderr,"n[%d]=%e\n",i,n[i]);
  //}
  //lb_calc_local_rho(&lbfluid[index]);
  //fprintf(stderr,"rho=%e\n",*(lbfluid[index].rho));

#else
  int i;

  /* In the general case, we don't know a priori which 
   * velocities propagate to higher or lower indices.
   * So we use a complete new array as buffer and
   * copy it afterwards (copying is necessary because
   * the halo communication uses fixed memory areas)
   * \todo Change the halo communication such that the pointers can be swapped!
   */ 

  double *n     = lbfluid[0].n;
  double *n_new = lbfluid[0].n_tmp;
  double (*c)[3] = lbmodel.c;

  /* calculate the index shift for all velocities */
  for (i=0;i<n_veloc;i++) {
    next[i] = n_veloc*(c[i][0]+yperiod*c[i][1]+zperiod*c[i][2]) + i;
  }

  /* propagate the populations */
  /* on the surface we have to check that shifts 
   * don't lead out of the cell's node range */
  index = 0;

  for (k=0; k<lblattice.halo_offset; k++) {
    for (i=0;i<n_veloc;i++) {
      if (index+next[i]>=0) {
	n_new[index+next[i]] = n[index+i];
      }
    }
    index += n_veloc;
  }

  for (k=lblattice.halo_offset;k<lblattice.halo_grid_volume-lblattice.halo_offset;k++) {
    for (i=0;i<n_veloc;i++) {
	n_new[index+next[i]] = n[index+i];
    }
    index += n_veloc;
  }

  for (k=lblattice.halo_grid_volume-lblattice.halo_offset;k<lblattice.halo_grid_volume;k++) {
    for (i=0;i<n_veloc;i++) {
      if (index+next[i]<lblattice.halo_grid_volume*n_veloc) {
	  n_new[index+next[i]] = n[index+i];
      }
    }
    index += n_veloc;
  } 

  memcpy(n,n_new,lblattice.halo_grid_volume*n_veloc*sizeof(double));

#endif

}
#endif

/*@}*/

/***********************************************************************/
/** \name External forces */
/***********************************************************************/
/*@{*/

/** Apply external forces to the fluid.
 *
 * Eq. (28) Ladd and Verberg, J. Stat. Phys. 104(5/6):1191 (2001).
 * Note that the second moment of the force is neglected.
 */
#if 0
#ifdef EXTERNAL_FORCES
MDINLINE void lb_external_forces() {

  int x, y, z, index;
  double *local_rho, *local_n, *local_j, *local_pi;
  double u[3], f[3], B[3], C[6];

  f[0] = lbpar.ext_force[0];
  f[1] = lbpar.ext_force[1];
  f[2] = lbpar.ext_force[2];

  index = lblattice.halo_offset;
  for (z=1; z<=lblattice.grid[2]; z++) {
    for (y=1; y<=lblattice.grid[1]; y++) {
      for (x=1; x<=lblattice.grid[0]; x++) {

#ifdef CONSTRAINTS
	if (lbfluid[index].boundary==0) 
#endif
	{

	  local_n   = lbfluid[index].n;
	  local_rho = lbfluid[index].rho;
	  local_j   = lbfluid[index].j;
	  local_pi  = lbfluid[index].pi;

	  lb_calc_local_fields(&lbfluid[index],0);
	  
	  u[0] = local_j[0]/(*local_rho);
	  u[1] = local_j[1]/(*local_rho);
	  u[2] = local_j[2]/(*local_rho);
	  
	  C[0] = (1.+gamma_shear)*u[0]*f[0] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
	  C[2] = (1.+gamma_shear)*u[1]*f[1] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
	  C[5] = (1.+gamma_shear)*u[2]*f[2] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
	  C[1] = 1./2.*(1.+gamma_shear)*(u[0]*f[1]+u[1]*f[0]);
	  C[3] = 1./2.*(1.+gamma_shear)*(u[0]*f[2]+u[2]*f[0]);
	  C[4] = 1./2.*(1.+gamma_shear)*(u[1]*f[2]+u[2]*f[1]);

	  //C[0] = C[1] = C[2] = C[3] = C[4] = C[5] = 0.0;

#if 0
	  local_j[0] += f[0];
	  local_j[1] += f[1];
	  local_j[2] += f[2];

	  local_pi[0] += C[0];
	  local_pi[1] += C[1];
	  local_pi[2] += C[2];
	  local_pi[3] += C[3];
	  local_pi[4] += C[4];
	  local_pi[5] += C[5];

	  lb_set_local_fields(&lbfluid[index],*local_rho,local_j,local_pi);
#endif

#ifdef D3Q19

#if 1
	  double tmp1, tmp2, tmp3, tmp4;

	  B[0] = f[0]/6.;
	  B[1] = f[1]/6.;
	  B[2] = f[2]/6.;

	  //C[0] *= 0.25;
	  //C[1] *= 0.25;
	  //C[2] *= 0.25;
	  //C[3] *= 0.25;
	  //C[4] *= 0.25;
	  //C[5] *= 0.25;
	  //trace /= 12.;

	  tmp1 = 0.5 * (C[0] + C[2] + C[5]);

	  local_n[0]  += - tmp1;

	  double sum3 = - tmp1;

	  tmp1 /= 6.;

	  local_n[1]  +=   B[0] + 0.25*C[0] - tmp1;
	  local_n[2]  += - B[0] + 0.25*C[0] - tmp1;
	  local_n[3]  +=   B[1] + 0.25*C[2] - tmp1;
	  local_n[4]  += - B[1] + 0.25*C[2] - tmp1;
	  local_n[5]  +=   B[2] + 0.25*C[5] - tmp1;
	  local_n[6]  += - B[2] + 0.25*C[5] - tmp1;
	  
	  tmp1 = 0.5*(B[0] + B[1]);
	  tmp2 = 0.5*(B[0] - B[1]);
	  tmp3 = (C[0] + C[2] - 0.5*C[5])/12.;
	  tmp4 = C[1]/4.;

	  sum3 += 4.*tmp3;

	  local_n[7]  +=   tmp1 + tmp3 + tmp4;
	  local_n[8]  += - tmp1 + tmp3 + tmp4;
	  local_n[9]  +=   tmp2 + tmp3 - tmp4;
	  local_n[10] += - tmp2 + tmp3 - tmp4;

	  tmp1 = 0.5*(B[0] + B[2]);
	  tmp2 = 0.5*(B[0] - B[2]);
	  tmp3 = (C[0] + C[5] - 0.5*C[2])/12.;
	  tmp4 = C[3]/4.;
	  
	  sum3 += 4.*tmp3;

	  local_n[11] +=   tmp1 + tmp3 + tmp4;
	  local_n[12] += - tmp1 + tmp3 + tmp4;
	  local_n[13] +=   tmp2 + tmp3 - tmp4;
	  local_n[14] += - tmp2 + tmp3 - tmp4;

	  tmp1 = 0.5*(B[1] + B[2]);
	  tmp2 = 0.5*(B[1] - B[2]);
	  tmp3 = (C[2] + C[5] - 0.5*C[0])/12.;
	  tmp4 = C[4]/4.;

	  sum3 += 4.*tmp3;

	  local_n[15] +=   tmp1 + tmp3 + tmp4;
	  local_n[16] += - tmp1 + tmp3 + tmp4;
	  local_n[17] +=   tmp2 + tmp3 - tmp4;
	  local_n[18] += - tmp2 + tmp3 - tmp4;

	  //fprintf(stderr,"%e %e\n",sum3,(C[0]+C[2]+C[5])/8.);
#endif


	  //int i;
	  //for (i=0;i<n_veloc;i++) {
	  //  fprintf(stderr," %e ",local_n[i]);
	  //}
	  //fprintf(stderr,"\n");

#if 0
	  //double delta_j[] = {0., 0., 0.};
	  double *delta_j = f;
	  //fprintf(stderr,"f=(%e,%e,%e)\n",delta_j[0],delta_j[1],delta_j[2]);
	  local_n[1]  +=   1./6.  * delta_j[0];
	  local_n[2]  += - 1./6.  * delta_j[0];
	  local_n[3]  +=   1./6.  * delta_j[1];
	  local_n[4]  += - 1./6.  * delta_j[1];
	  local_n[5]  +=   1./6.  * delta_j[2];
	  local_n[6]  += - 1./6.  * delta_j[2];
	  local_n[7]  +=   1./12. * (delta_j[0]+delta_j[1]);
	  local_n[8]  += - 1./12. * (delta_j[0]+delta_j[1]);
	  local_n[9]  +=   1./12. * (delta_j[0]-delta_j[1]);
	  local_n[10] += - 1./12. * (delta_j[0]-delta_j[1]);
	  local_n[11] +=   1./12. * (delta_j[0]+delta_j[2]);
	  local_n[12] += - 1./12. * (delta_j[0]+delta_j[2]);
	  local_n[13] +=   1./12. * (delta_j[0]-delta_j[1]);
	  local_n[14] += - 1./12. * (delta_j[0]-delta_j[1]);
	  local_n[15] +=   1./12. * (delta_j[1]+delta_j[2]);
	  local_n[16] += - 1./12. * (delta_j[1]+delta_j[2]);
	  local_n[17] +=   1./12. * (delta_j[1]-delta_j[2]);
	  local_n[18] += - 1./12. * (delta_j[1]-delta_j[2]);
#endif

#else
	  int i;
	  double tmp=0.0;
	  double (*c)[3] = lbmodel.c;
	  double (*coeff)[4] = lbmodel.coeff;

	  for (i=0; i<n_veloc; i++) {

	    tmp = C[0]*c[i][0]*c[i][0]
	      + (2.*C[1]*c[i][0]+C[2]*c[i][1])*c[i][1]
	      + (2.*C[3]*c[i][0]+C[4]*c[i][1]+C[5]*c[i][2])*c[i][2];

	    local_n[i] += coeff[i][1] * scalar(f,c[i]);
	    local_n[i] += coeff[i][2] * tmp;
	    local_n[i] += coeff[i][3] * (C[0]+C[2]+C[5])/3.0;

	  }
#endif

	}
	++index;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

}
#endif
#endif
/*@}*/

MDINLINE void lb_sc_calc_modes(int index, double *mode) {

  int yperiod = lblattice.halo_grid[0];
  int zperiod = lblattice.halo_grid[0]*lblattice.halo_grid[1];

  double n[19];
  n[0]  = lbfluid[index].n[0];
  n[1]  = lbfluid[index-1].n[1];
  n[2]  = lbfluid[index+1].n[2];
  n[3]  = lbfluid[index-yperiod].n[3];
  n[4]  = lbfluid[index+yperiod].n[4];
  n[5]  = lbfluid[index-zperiod].n[5];
  n[6]  = lbfluid[index+zperiod].n[6];
  n[7]  = lbfluid[index-(1+yperiod)].n[7];
  n[8]  = lbfluid[index+(1+yperiod)].n[8];
  n[9]  = lbfluid[index-(1-yperiod)].n[9];
  n[10] = lbfluid[index+(1-yperiod)].n[10];
  n[11] = lbfluid[index-(1+zperiod)].n[11];
  n[12] = lbfluid[index+(1+zperiod)].n[12];
  n[13] = lbfluid[index-(1-zperiod)].n[13];
  n[14] = lbfluid[index+(1-zperiod)].n[14];
  n[15] = lbfluid[index-(yperiod+zperiod)].n[15];
  n[16] = lbfluid[index+(yperiod+zperiod)].n[16];
  n[17] = lbfluid[index-(yperiod-zperiod)].n[17];
  n[18] = lbfluid[index+(yperiod-zperiod)].n[18];

  //if (index==15888) {
  //  int i;
  //  for (i=0;i<n_veloc;i++) fprintf(stderr,"n[%d] = %e\n",i,n[i]);
  //}

  /* mass mode */
  mode[ 0] =   n[ 0] + n[ 1] + n[ 2] + n[ 3] + n[4] + n[5] + n[6]
             + n[ 7] + n[ 8] + n[ 9] + n[10]
             + n[11] + n[12] + n[13] + n[14]
             + n[15] + n[16] + n[17] + n[18];

  /* momentum modes */
  mode[ 1] =   n[ 1] - n[ 2] 
             + n[ 7] - n[ 8] + n[ 9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[ 2] =   n[ 3] - n[ 4]
             + n[ 7] - n[ 8] - n[ 9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[ 3] =   n[ 5] - n[ 6]
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];

  /* stress modes */
  mode[ 4] = - n[ 0] 
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18];
  mode[ 5] =   n[ 1] + n[ 2] - n[ 3] - n[4]
             + n[11] + n[12] + n[13] + n[14] - n[15] - n[16] - n[17] - n[18];
  mode[ 6] =   n[ 1] + n[ 2] + n[ 3] + n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             - 2.*(n[5] + n[6] - n[7] - n[8] - n[9] - n[10]);
  mode[ 7] =   n[ 7] + n[ 8] - n[ 9] - n[10];
  mode[ 8] =   n[11] + n[12] - n[13] - n[14];
  mode[ 9] =   n[15] + n[16] - n[17] - n[18];

  /* ghost modes */
  mode[10] = 2.*(n[2] - n[1]) 
             + n[7] - n[8] + n[9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[11] = 2.*(n[4] - n[3])
             + n[7] - n[8] - n[9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[12] = 2.*(n[6] - n[5])
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];
  mode[13] =   n[ 7] - n[ 8] + n[ 9] - n[10] - n[11] + n[12] - n[13] + n[14];
  mode[14] =   n[ 7] - n[ 8] - n[ 9] + n[10] - n[15] + n[16] - n[17] + n[18];
  mode[15] =   n[11] - n[12] - n[13] + n[14] - n[15] + n[16] + n[17] - n[18];
  mode[16] =   n[ 0]
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18]
             - 2.*(n[1] + n[2] + n[3] + n[4] + n[5] + n[6]);
  mode[17] =   n[ 3] + n[ 4] - n[ 1] - n[ 2] 
             + n[11] + n[12] + n[13] + n[14] 
             - n[15] - n[16] - n[17] - n[18];
  mode[18] = - n[ 1] - n[ 2] - n[ 3] - n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             + 2.*(n[5] + n[6] + n[7] + n[8] + n[9] + n[10]);

}

MDINLINE void lb_relax_modes(double *mode) {

  double rho, j[3], pi_eq[6];

  /* re-construct the real density 
   * remember that the populations are stored as differences to their
   * equilibrium value */
  rho = mode[0] + lbpar.rho;

  j[0] = mode[1];
  j[1] = mode[2];
  j[2] = mode[3];

#ifdef EXTERNAL_FORCES
  /* if external forces are present, the momentum density is
   * redefined to inlcude one half-step of the force action.
   * See the Chapman-Enskog expansion in [Ladd & Verberg]. */
  j[0] += 0.5*lbpar.ext_force[0];
  j[1] += 0.5*lbpar.ext_force[1];
  j[2] += 0.5*lbpar.ext_force[2];
#endif

  /* equilibrium part of the stress modes */
  pi_eq[0] = scalar(j,j)/rho;
  pi_eq[1] = (SQR(j[0])-SQR(j[1]))/rho;
  pi_eq[2] = (scalar(j,j) - 3.0*SQR(j[2]))/rho;
  pi_eq[3] = j[0]*j[1]/rho;
  pi_eq[4] = j[0]*j[2]/rho;
  pi_eq[5] = j[1]*j[2]/rho;

  /* relax the stress modes */  
  mode[4] = pi_eq[0] + gamma_bulk*(mode[4] - pi_eq[0]);
  mode[5] = pi_eq[1] + gamma_shear*(mode[5] - pi_eq[1]);
  mode[6] = pi_eq[2] + gamma_shear*(mode[6] - pi_eq[2]);
  mode[7] = pi_eq[3] + gamma_shear*(mode[7] - pi_eq[3]);
  mode[8] = pi_eq[4] + gamma_shear*(mode[8] - pi_eq[4]);
  mode[9] = pi_eq[5] + gamma_shear*(mode[9] - pi_eq[5]);

  /* relax the ghost modes (project them out) */
  /* ghost modes have no equilibrium part due to orthogonality */
  mode[10] = 0.0;
  mode[11] = 0.0;
  mode[12] = 0.0;
  mode[13] = 0.0;
  mode[14] = 0.0;
  mode[15] = 0.0;
  mode[16] = 0.0;
  mode[17] = 0.0;
  mode[18] = 0.0;

}

MDINLINE void lb_thermalize_modes(double *mode) {
    double rootrho = sqrt(mode[0]+lbpar.rho);
    double fluct[6];

    /* stress modes */
    mode[4] += (fluct[0] = rootrho*lb_phi[4]*gaussian_random());
    mode[5] += (fluct[1] = rootrho*lb_phi[5]*gaussian_random());
    mode[6] += (fluct[2] = rootrho*lb_phi[6]*gaussian_random());
    mode[7] += (fluct[3] = rootrho*lb_phi[7]*gaussian_random());
    mode[8] += (fluct[4] = rootrho*lb_phi[8]*gaussian_random());
    mode[9] += (fluct[5] = rootrho*lb_phi[9]*gaussian_random());
    //if (node == &lbfluid[lblattice.halo_offset]) {
    //  fprintf(stderr,"%f %f %f %f %f %f\n",fluct[0],fluct[1],fluct[2],fluct[3],fluct[4],fluct[5]);
    //}
    
    /* ghost modes */
    mode[10] += rootrho*lb_phi[10]*gaussian_random();
    mode[11] += rootrho*lb_phi[11]*gaussian_random();
    mode[12] += rootrho*lb_phi[12]*gaussian_random();
    mode[13] += rootrho*lb_phi[13]*gaussian_random();
    mode[14] += rootrho*lb_phi[14]*gaussian_random();
    mode[15] += rootrho*lb_phi[15]*gaussian_random();
    mode[16] += rootrho*lb_phi[16]*gaussian_random();
    mode[17] += rootrho*lb_phi[17]*gaussian_random();
    mode[18] += rootrho*lb_phi[18]*gaussian_random();

#ifdef ADDITIONAL_CHECKS
    rancounter += 15;
#endif
}

MDINLINE void lb_external_forces(double* mode) {

#ifdef EXTERNAL_FORCES
  double rho, f[3], u[3], C[6];
  
  f[0] = lbpar.ext_force[0];
  f[1] = lbpar.ext_force[1];
  f[2] = lbpar.ext_force[2];

  rho = mode[0] + lbpar.rho;

  /* hydrodynamic momentum density is redefined when external forces present */
  u[0] = (mode[1] + 0.5*f[0])/rho;
  u[1] = (mode[2] + 0.5*f[1])/rho;
  u[2] = (mode[3] + 0.5*f[2])/rho;

  C[0] = (1.+gamma_bulk)*u[0]*f[0] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[2] = (1.+gamma_bulk)*u[1]*f[1] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[5] = (1.+gamma_bulk)*u[2]*f[2] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[1] = 1./2.*(1.+gamma_shear)*(u[0]*f[1]+u[1]*f[0]);
  C[3] = 1./2.*(1.+gamma_shear)*(u[0]*f[2]+u[2]*f[0]);
  C[4] = 1./2.*(1.+gamma_shear)*(u[1]*f[2]+u[2]*f[1]);

  /* update momentum modes */
  mode[1] += f[0];
  mode[2] += f[1];
  mode[3] += f[2];

  /* update stress modes */
  mode[4] += C[0] + C[2] + C[5];
  mode[5] += 2.*C[0] - C[2] - C[5];
  mode[6] += C[2] - C[5];
  mode[7] += C[1];
  mode[8] += C[3];
  mode[9] += C[4];
#endif

}

MDINLINE void lb_calc_n_from_modes(int index, double *mode) {

  double *n = lbfluid_new[index].n;
  double *w = lbmodel.w;
  double m[19];
  int i;

  //  fprintf(stderr,"%p %p\n",lbfluid[lblattice.halo_grid_volume+index].n,lbfluid_new[index].n);

  //if (index==15888) fprintf(stderr,"j = %e\n",mode[1]);

#ifdef D3Q19
  double (*e)[19] = d3q19_modebase;

  /* normalization factors enter in the back transformation */
  for (i=0;i<n_veloc;i++) {
    m[i] = 1./e[19][i]*mode[i];
  }

  n[ 0] = m[0] - m[4] + m[16];
  n[ 1] = m[0] + m[1] + m[5] + m[6] - m[17] - m[18] - 2.*(m[10] + m[16]);
  n[ 2] = m[0] - m[1] + m[5] + m[6] - m[17] - m[18] + 2.*(m[10] - m[16]);
  n[ 3] = m[0] + m[2] - m[5] + m[6] + m[17] - m[18] - 2.*(m[11] + m[16]);
  n[ 4] = m[0] - m[2] - m[5] + m[6] + m[17] - m[18] + 2.*(m[11] - m[16]);
  n[ 5] = m[0] + m[3] - 2.*(m[6] + m[12] + m[16] - m[18]);
  n[ 6] = m[0] - m[3] - 2.*(m[6] - m[12] + m[16] - m[18]);
  n[ 7] = m[0] + m[ 1] + m[ 2] + m[ 4] + 2.*m[6]
        + m[7] + m[10] + m[11] + m[13] + m[14] + m[16] + 2.*m[18];
  n[ 8] = m[0] - m[ 1] - m[ 2] + m[ 4] + 2.*m[6]
        + m[7] - m[10] - m[11] - m[13] - m[14] + m[16] + 2.*m[18];
  n[ 9] = m[0] + m[ 1] - m[ 2] + m[ 4] + 2.*m[6]
        - m[7] + m[10] - m[11] + m[13] - m[14] + m[16] + 2.*m[18];
  n[10] = m[0] - m[ 1] + m[ 2] + m[ 4] + 2.*m[6]
        - m[7] - m[10] + m[11] - m[13] + m[14] + m[16] + 2.*m[18];
  n[11] = m[0] + m[ 1] + m[ 3] + m[ 4] + m[ 5] - m[ 6]
        + m[8] + m[10] + m[12] - m[13] + m[15] + m[16] + m[17] - m[18];
  n[12] = m[0] - m[ 1] - m[ 3] + m[ 4] + m[ 5] - m[ 6]
        + m[8] - m[10] - m[12] + m[13] - m[15] + m[16] + m[17] - m[18];
  n[13] = m[0] + m[ 1] - m[ 3] + m[ 4] + m[ 5] - m[ 6]
        - m[8] + m[10] - m[12] - m[13] - m[15] + m[16] + m[17] - m[18];
  n[14] = m[0] - m[ 1] + m[ 3] + m[ 4] + m[ 5] - m[ 6]
        - m[8] - m[10] + m[12] + m[13] + m[15] + m[16] + m[17] - m[18];
  n[15] = m[0] + m[ 2] + m[ 3] + m[ 4] - m[ 5] - m[ 6]
        + m[9] + m[11] + m[12] - m[14] - m[15] + m[16] - m[17] - m[18];
  n[16] = m[0] - m[ 2] - m[ 3] + m[ 4] - m[ 5] - m[ 6]
        + m[9] - m[11] - m[12] + m[14] + m[15] + m[16] - m[17] - m[18];
  n[17] = m[0] + m[ 2] - m[ 3] + m[ 4] - m[ 5] - m[ 6]
        - m[9] + m[11] - m[12] - m[14] + m[15] + m[16] - m[17] - m[18];
  n[18] = m[0] - m[ 2] + m[ 3] + m[ 4] - m[ 5] - m[ 6]
        - m[9] - m[11] + m[12] + m[14] - m[15] + m[16] - m[17] - m[18];

  /* weights enter in the back transformation */
  for (i=0;i<n_veloc;i++) {
    n[i] = w[i]*n[i];
  }

#else
  int j;
  double n2[n_veloc];
  for (i=0; i<n_veloc;i++) {
    n2[i] = 0.0;
    for (j=0;j<n_veloc;j++) {
      n2[i] += 1./e[19][j]*mode[j]*e[j][i];
    }
    n2[i] = w[i]*(n2[i]);
  }

  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"n[%d]=%f n2[%d]=%f\n",i,n[i],i,n2[i]);
  }
#endif

}

MDINLINE void lb_stream_collide(int index) {

  double modes[19];

  //if (index==15888) fprintf(stderr,"n[1] = %e\n",lbfluid[index-1].n[1]);

  lb_sc_calc_modes(index, modes);

  //if (index==15888) fprintf(stderr,"j = %e\n",modes[1]);

  lb_relax_modes(modes);

  lb_external_forces(modes);

  if (fluct) lb_thermalize_modes(modes);

  lb_calc_n_from_modes(index, modes);

  //if (index==15888) fprintf(stderr,"n[1] = %e\n",lbfluid_new[index].n[1]);

}

/***********************************************************************/
/** \name Update step for the lattice Boltzmann fluid                  */
/***********************************************************************/
/*@{*/

/** Update the lattice Boltzmann fluid.  
 *
 * This function is called from the integrator. Since the time step
 * for the lattice dynamics can be coarser than the MD time step, we
 * monitor the time since the last lattice update.
 */
void lattice_boltzmann_update() {

  int x, y, z, index;
  LB_FluidNode* tmp;

  fluidstep += time_step;

  if (fluidstep>=tau) {

    fluidstep=0.0;

    /* exchange halo regions */
    halo_communication(&update_halo_comm[toggle]);
#ifdef ADDITIONAL_CHECKS
    lb_check_halo_regions();
#endif

    /* loop over all lattice cells (halo_excluded) */
    index = lblattice.halo_offset;
    for (z=1; z<=lblattice.grid[2]; z++) {
      for (y=1; y<=lblattice.grid[1]; y++) {
	for (x=1; x<=lblattice.grid[0]; x++) {
	  
	  lb_stream_collide(index);

	  ++index; /* next node */
	}

	index += 2; /* skip halo region */	
      }

      index += 2*lblattice.halo_grid[0]; /* skip halo region */
    }

    /* toggle the indicator for old and new population fields */
    toggle = (toggle+1)%2;

    /* swap the pointers */
    //fprintf(stderr,"swapping pointers\n");
    tmp = lbfluid;
    lbfluid = lbfluid_new;
    lbfluid_new = tmp;

  }
  
}

/***********************************************************************/
/** \name Coupling part */
/***********************************************************************/
/*@{*/

/** Transfer a certain amount of momentum to a elementray cell of fluid.
 * 
 * Eq. (14) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
 *
 * @param momentum   Momentum to be transfered to the fluid (lattice
 *                   units) (Input).
 * @param node_index Indices of the sites of the elementary lattice
 *                   cell (Input).
 * @param delta      Weights for the assignment to the single lattice
 *                   sites (Input).
 * @param badrandoms Flag/Counter for the occurrence negative
 *                   populations (Output).
 */
MDINLINE void lb_transfer_momentum(const double momentum[3], const int node_index[8], const double delta[6]) {

  int x, y, z, index;
  LB_FluidNode *local_node;
  double *local_n, *n_tmp;
  double *local_j, delta_j[3];

  /* We don't need to save the local populations because 
   * we use a trick for their restoration:
   * We substract the old random force from the new one,
   * hence the previous change in the local populations
   * is automatically revoked during the recalculation.
   * Note that this makes it necessary to actually apply 
   * all changes and forbids to return immediately when negative
   * populations occur.
   */

  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {
	
	index = node_index[(z*2+y)*2+x];
	local_node = &lbfluid[index];
	local_n = n_tmp = local_node->n;
	local_j = local_node->j;

	delta_j[0] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[0];
	delta_j[1] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[1];
	delta_j[2] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[2];

#ifdef D3Q19
	local_n[1]  = n_tmp[1]  + 1./6.*delta_j[0];
	local_n[2]  = n_tmp[2]  - 1./6.*delta_j[0];
	local_n[3]  = n_tmp[3]  + 1./6.*delta_j[1];
	local_n[4]  = n_tmp[4]  - 1./6.*delta_j[1];
	local_n[5]  = n_tmp[5]  + 1./6.*delta_j[2];
	local_n[6]  = n_tmp[6]  - 1./6.*delta_j[2];
	local_n[7]  = n_tmp[7]  + 1./12.*(delta_j[0]+delta_j[1]);
	local_n[8]  = n_tmp[8]  - 1./12.*(delta_j[0]+delta_j[1]);
	local_n[9]  = n_tmp[9]  + 1./12.*(delta_j[0]-delta_j[1]);
	local_n[10] = n_tmp[10] - 1./12.*(delta_j[0]-delta_j[1]);
	local_n[11] = n_tmp[11] + 1./12.*(delta_j[0]+delta_j[2]);
	local_n[12] = n_tmp[12] - 1./12.*(delta_j[0]+delta_j[2]);
	local_n[13] = n_tmp[13] + 1./12.*(delta_j[0]-delta_j[2]);
	local_n[14] = n_tmp[14] - 1./12.*(delta_j[0]-delta_j[2]);
	local_n[15] = n_tmp[15] + 1./12.*(delta_j[1]+delta_j[2]);
	local_n[16] = n_tmp[16] - 1./12.*(delta_j[1]+delta_j[2]);
	local_n[17] = n_tmp[17] + 1./12.*(delta_j[1]-delta_j[2]);
	local_n[18] = n_tmp[18] - 1./12.*(delta_j[1]-delta_j[2]);
#else
	int i;
	double (*c)[3] = lbmodel.c;
	double (*coeff)[4] = lbmodel.coeff;

	for (i=0;i<n_veloc;i++) {
	  local_n[i] = n_tmp[i] + coeff[i][1] * scalar(delta_j,c[i]);
	}

#endif

#ifdef ADDITIONAL_CHECKS
	//lb_check_negative_n(local_node);
#endif

      }
    }
  }

}

/** Coupling of a particle to viscous fluid with Stokesian friction.
 * 
 * Section II.C. Ahlrichs and Duenweg, JCP 111(17):8225 (1999)
 *
 * @param p          The coupled particle (Input).
 * @param force      Coupling force between particle and fluid (Output).
 */
MDINLINE void lb_viscous_momentum_exchange(Particle *p, double force[3]) {

  int x,y,z;
  int node_index[8];
  double delta[6];
  LB_FluidNode *local_node;
  double *local_rho, *local_j, interpolated_u[3], delta_j[3];
#ifdef ADDITIONAL_CHECKS
  double old_rho[8];
#endif

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: f = (%.3e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));

  /* determine elementary lattice cell surrounding the particle 
     and the relative position of the particle in this cell */ 
  map_position_to_lattice(&lblattice,p->r.p,node_index,delta) ;

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB delta=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f) pos=(%.3f,%.3f,%.3f)\n",this_node,delta[0],delta[1],delta[2],delta[3],delta[4],delta[5],p->r.p[0],p->r.p[1],p->r.p[2]));

  /* calculate fluid velocity at particle's position
     this is done by linear interpolation
     (Eq. (11) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  interpolated_u[0] = interpolated_u[1] = interpolated_u[2] = 0.0 ;
  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {

	local_node = &lbfluid[node_index[(z*2+y)*2+x]];
	local_rho  = local_node->rho;
	local_j    = local_node->j;

#ifdef ADDITIONAL_CHECKS
	old_rho[(z*2+y)*2+x] = *local_rho;
#endif

	interpolated_u[0] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[0]/(*local_rho);
	interpolated_u[1] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[1]/(*local_rho);	  
	interpolated_u[2] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[2]/(*local_rho) ;

      }
    }
  }
  
  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB u = (%.16e,%.3e,%.3e) v = (%.16e,%.3e,%.3e)\n",this_node,interpolated_u[0],interpolated_u[1],interpolated_u[2],p->m.v[0],p->m.v[1],p->m.v[2]));

  /* calculate viscous force
   * take care to rescale velocities with time_step and transform to MD units 
   * (Eq. (9) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  force[0] = - lbpar.friction * (p->m.v[0]/time_step - interpolated_u[0]*agrid/tau);
  force[1] = - lbpar.friction * (p->m.v[1]/time_step - interpolated_u[1]*agrid/tau);
  force[2] = - lbpar.friction * (p->m.v[2]/time_step - interpolated_u[2]*agrid/tau);

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_drag = (%.6e,%.3e,%.3e)\n",this_node,force[0],force[1],force[2]));

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_random = (%.6e,%.3e,%.3e)\n",this_node,p->lc.f_random[0],p->lc.f_random[1],p->lc.f_random[2]));

  force[0] = force[0] + p->lc.f_random[0];
  force[1] = force[1] + p->lc.f_random[1];
  force[2] = force[2] + p->lc.f_random[2];

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_tot = (%.6e,%.3e,%.3e)\n",this_node,force[0],force[1],force[2]));
      
  /* transform momentum transfer to lattice units
     (Eq. (12) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  delta_j[0] = - force[0]*time_step*tau/agrid;
  delta_j[1] = - force[1]*time_step*tau/agrid;
  delta_j[2] = - force[2]*time_step*tau/agrid;
    
  lb_transfer_momentum(delta_j,node_index,delta);

#ifdef ADDITIONAL_CHECKS
  int i;
  for (i=0;i<8;i++) {
    lb_calc_local_rho(&lbfluid[node_index[i]]);
    local_rho = lbfluid[node_index[i]].rho;
    if (fabs(*local_rho-old_rho[i]) > ROUND_ERROR_PREC) {
      char *errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt,"{108 Mass loss/gain %le in lb_viscous_momentum_exchange for particle %d} ",*local_rho-old_rho[i],p->p.identity);
    }
  }
#endif

}

/** Calculate particle lattice interactions.
 * So far, only viscous coupling with Stokesian friction is
 * implemented.
 * Include all particle-lattice forces in this function.
 * The function is called from \ref force_calc.
 *
 * Parallelizing the fluid particle coupling is not straightforward
 * because drawing of random numbers makes the whole thing nonlocal.
 * One way to do it is to treat every particle only on one node, i.e.
 * the random numbers need not be communicated. The particles that are 
 * not fully inside the local lattice are taken into account via their
 * ghost images on the neighbouring nodes. But this requires that the 
 * correct values of the surrounding lattice nodes are available on 
 * the respective node, which means that we have to communicate the 
 * halo regions before treating the ghost particles. Moreover, after 
 * determining the ghost couplings, we have to communicate back the 
 * halo region such that all local lattice nodes have the correct values.
 * Thus two communication phases are involved which will most likely be 
 * the bottleneck of the computation.
 *
 * Another way of dealing with the particle lattice coupling is to 
 * treat a particle and all of it's images explicitly. This requires the
 * communication of the random numbers used in the calculation of the 
 * coupling force. The problem is now that, if random numbers have to 
 * be redrawn, we cannot efficiently determine which particles and which 
 * images have to be re-calculated. We therefore go back to the outset
 * and go through the whole system again until no failure occurs during
 * such a sweep. In the worst case, this is very inefficient because
 * many things are recalculated although they actually don't need.
 * But we can assume that this happens extremely rarely and then we have
 * on average only one communication phase for the random numbers, which
 * probably makes this method preferable compared to the above one.
 */
void calc_particle_lattice_ia() {
 
  int i, k, c, np;
  Cell *cell ;
  Particle *p ;
  double force[3];

  if (transfer_momentum) {

    /* exchange halo regions */
    halo_communication(&update_halo_comm[toggle]) ;
#ifdef ADDITIONAL_CHECKS
    //fprintf(stderr,"calc_particle_lattice_ia() checking halos %d\n",toggle);
    lb_check_halo_regions();
#endif
    
    for (k=0;k<lblattice.halo_grid_volume;k++) {
      lb_calc_local_fields(&lbfluid[k],0);
    }

    /* draw random numbers for local particles */
    for (c=0;c<local_cells.n;c++) {
      cell = local_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;
      for (i=0;i<np;i++) {
	p[i].lc.f_random[0] = -lb_coupl_pref*(d_random()-0.5);
	p[i].lc.f_random[1] = -lb_coupl_pref*(d_random()-0.5);
	p[i].lc.f_random[2] = -lb_coupl_pref*(d_random()-0.5);

#ifdef ADDITIONAL_CHECKS
	rancounter += 3;
#endif
      }
    }
    
    /* communicate the random numbers */
    ghost_communicator(&cell_structure.ghost_lbcoupling_comm) ;
    
    /* local cells */
    for (c=0;c<local_cells.n;c++) {
      cell = local_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {

	lb_viscous_momentum_exchange(&p[i],force) ;

	/* add force to the particle */
	p[i].f.f[0] += force[0];
	p[i].f.f[1] += force[1];
	p[i].f.f[2] += force[2];

	ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f = (%.6e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));
  
      }

    }

    /* ghost cells */
    for (c=0;c<ghost_cells.n;c++) {
      cell = ghost_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {
	/* for ghost particles we have to check if they lie
	 * in the range of the local lattice nodes */
	if (p[i].r.p[0] >= my_left[0]-lblattice.agrid && p[i].r.p[0] < my_right[0]
	    && p[i].r.p[1] >= my_left[1]-lblattice.agrid && p[i].r.p[1] < my_right[1]
	    && p[i].r.p[2] >= my_left[2]-lblattice.agrid && p[i].r.p[2] < my_right[2]) {

	  ONEPART_TRACE(if(p[i].p.identity==check_id) fprintf(stderr,"%d: OPT: LB coupling of ghost particle:\n",this_node));

	  lb_viscous_momentum_exchange(&p[i],force) ;

	  /* ghosts must not have the force added! */

	  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f = (%.6e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));

	}
      }
    }
    
  }

}

/***********************************************************************/

/** Calculate the average density of the fluid in the system.
 * This function has to be called after changing the density of
 * a local lattice site in order to set lbpar.rho consistently. */
void lb_calc_average_rho() {

  int x, y, z, index;
  double rho, sum_rho;

  rho = 0.0;
  index = 0;
  for (z=1; z<=lblattice.grid[2]; z++) {
    for (y=1; y<=lblattice.grid[1]; y++) {
      for (x=1; x<=lblattice.grid[0]; x++) {
	
	lb_calc_local_rho(&lbfluid[index]);
	rho += *lbfluid[index].rho;

	index++;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

  MPI_Allreduce(&rho, &sum_rho, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  /* calculate average density in MD units */
  lbpar.rho = sum_rho / (box_l[0]*box_l[1]*box_l[2]);

}

/** Returns the hydrodynamic fields of a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Output)
 * @param j     Local momentum of the fluid (Output)
 * @param pi    Local stress tensor of the fluid (Output)
 */
void lb_get_local_fields(LB_FluidNode *node, double *rho, double *j, double *pi) {

  int i,k,m;

  double *local_rho = node->rho;
  double *local_j   = node->j;
  double *local_pi  = node->pi;

  lb_calc_local_fields(node,1);

  *rho = *local_rho;
  m = 0;
  for (i=0;i<3;i++) {
    j[i] = local_j[i];
    for (k=0;k<=i;k++) {
      pi[m] = local_pi[m];
      m++;
    }
  }

}

/** Sets the hydrodynamic fields on a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Input)
 * @param v     Local velocity of the fluid (Input)
 */
void lb_set_local_fields(LB_FluidNode *node, const double rho, const double *v, const double *pi) {

  double *local_rho = node->rho;
  double *local_j   = node->j;
  double *local_pi  = node->pi;

  *local_rho = rho;

  local_j[0] = rho * v[0];
  local_j[1] = rho * v[1];
  local_j[2] = rho * v[2];
  
  local_pi[0] = pi[0];
  local_pi[1] = pi[1];
  local_pi[2] = pi[2];
  local_pi[3] = pi[3];
  local_pi[4] = pi[4];
  local_pi[5] = pi[5];

  /* calculate populations according to equilibrium distribution */
  lb_calc_n_equilibrium(node);

}

/*@}*/

/***********************************************************************/
/** \name TCL stuff */
/***********************************************************************/

static int lb_parse_set_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  int k, index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 4 ;
  if (argc < 4) return TCL_ERROR ;
  if (!ARG0_IS_D(rho)) return TCL_ERROR ;
  for (k=0;k<3;k++) {
    if (!ARG_IS_D(k+1,j[k])) return TCL_ERROR ;
  }
    
  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);

  /* transform to lattice units */
  rho  *= agrid*agrid*agrid;
  j[0] *= tau/agrid;
  j[1] *= tau/agrid;
  j[2] *= tau/agrid;

  pi[0] = rho*lbmodel.c_sound_sq + j[0]*j[0]/rho;
  pi[2] = rho*lbmodel.c_sound_sq + j[1]*j[1]/rho;
  pi[5] = rho*lbmodel.c_sound_sq + j[2]*j[2]/rho;
  pi[1] = j[0]*j[1]/rho;
  pi[3] = j[0]*j[2]/rho;
  pi[4] = j[1]*j[2]/rho;

  mpi_send_fluid(node,index,rho,j,pi) ;

  lb_calc_average_rho();
  lb_reinit_parameters();

  return TCL_OK ;

}

static int lb_print_local_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  char buffer[256+4*TCL_DOUBLE_SPACE+3*TCL_INTEGER_SPACE];
  int index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 0;

  sprintf(buffer, "%d", ind[0]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[1]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[2]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);

  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);
  
  mpi_recv_fluid(node,index,&rho,j,pi) ;

  /* transform to MD units */
  rho  *= 1./(agrid*agrid*agrid);
  j[0] *= agrid/tau;
  j[1] *= agrid/tau;
  j[2] *= agrid/tau;

  Tcl_PrintDouble(interp, rho, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[0], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[1], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[2], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
    
  return TCL_OK ;

}

MDINLINE void lbnode_print_rho(Tcl_Interp *interp, double rho) {
  char buffer[TCL_DOUBLE_SPACE];

  Tcl_PrintDouble(interp, rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);

}

MDINLINE void lbnode_print_v(Tcl_Interp *interp, double *j, double rho) {
  char buffer[TCL_DOUBLE_SPACE];
  
  Tcl_PrintDouble(interp, j[0]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, j[1]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, j[2]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL); 

}

MDINLINE void lbnode_print_pi(Tcl_Interp *interp, double *pi) {
  char buffer[TCL_DOUBLE_SPACE];

  Tcl_PrintDouble(interp, pi[0], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[1], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[2], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[3], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[4], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[5], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
      
}

MDINLINE void lbnode_print_pi_neq(Tcl_Interp *interp, double rho, double *j, double *pi) {
  char buffer[TCL_DOUBLE_SPACE];
  double pi_neq[6];

  pi_neq[0] = pi[0] - rho*lbmodel.c_sound_sq - j[0]*j[0]/rho;
  pi_neq[2] = pi[2] - rho*lbmodel.c_sound_sq - j[1]*j[1]/rho;
  pi_neq[5] = pi[5] - rho*lbmodel.c_sound_sq - j[2]*j[2]/rho;
  pi_neq[1] = pi[1] - j[0]*j[1]/rho;
  pi_neq[3] = pi[3] - j[0]*j[2]/rho;
  pi_neq[4] = pi[4] - j[1]*j[2]/rho;

  Tcl_PrintDouble(interp, pi_neq[0], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[1], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[2], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[3], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[4], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[5], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);

}

static int lbnode_parse_print(Tcl_Interp *interp, int argc, char **argv, int *ind) {
  int node, index, grid[3];
  double rho, j[3], pi[6];

  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);
  
  mpi_recv_fluid(node,index,&rho,j,pi);

  while (argc > 0) {
    if (ARG0_IS_S("rho") || ARG0_IS_S("density")) 
      lbnode_print_rho(interp, rho);
    else if (ARG0_IS_S("u") || ARG0_IS_S("v") || ARG0_IS_S("velocity"))
      lbnode_print_v(interp, j, rho);
    else if (ARG0_IS_S("pi") || ARG0_IS_S("pressure"))
      lbnode_print_pi(interp, pi);
    else if (ARG0_IS_S("pi_neq")) /* this has to come after pi */
      lbnode_print_pi_neq(interp, rho, j, pi);
    else {
      Tcl_ResetResult(interp);
      Tcl_AppendResult(interp, "unknown fluid data \"", argv[0], "\" requested", (char *)NULL);
      return TCL_ERROR;
    }
    --argc; ++argv;
  }

  return TCL_OK;
}

static int lbfluid_parse_tau(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double tau;

    if (argc < 1) {
	Tcl_AppendResult(interp, "tau requires 1 argument", NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(tau)) {
	Tcl_AppendResult(interp, "wrong  argument for tau", (char *)NULL);
	return TCL_ERROR;
    }
    if (tau < 0.0) {
	Tcl_AppendResult(interp, "tau must be positive", (char *)NULL);
	return TCL_ERROR;
    }
    else if ((time_step >= 0.0) && (tau < time_step)) {
      Tcl_AppendResult(interp, "tau must be larger than MD time_step", (char *)NULL);
      return TCL_ERROR;
    }

    *change = 1;
    lbpar.tau = tau;

    mpi_bcast_lb_params(LBPAR_TAU);

    return TCL_OK;
}

static int lbfluid_parse_agrid(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    if (argc < 1) {
	Tcl_AppendResult(interp, "agrid requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(agrid)) {
	Tcl_AppendResult(interp, "wrong argument for agrid", (char *)NULL);
	return TCL_ERROR;
    }
    if (agrid <= 0.0) {
	Tcl_AppendResult(interp, "agrid must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.agrid = agrid;

    mpi_bcast_lb_params(LBPAR_AGRID);
 
    return TCL_OK;
}

static int lbfluid_parse_density(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double density;

    if (argc < 1) {
	Tcl_AppendResult(interp, "density requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(density)) {
	Tcl_AppendResult(interp, "wrong argument for density", (char *)NULL);
	return TCL_ERROR;
    }
    if (density <= 0.0) {
	Tcl_AppendResult(interp, "density must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.rho = density;

    mpi_bcast_lb_params(LBPAR_DENSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_viscosity(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double viscosity;

    if (argc < 1) {
	Tcl_AppendResult(interp, "viscosity requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(viscosity)) {
	Tcl_AppendResult(interp, "wrong argument for viscosity", (char *)NULL);
	return TCL_ERROR;
    }
    if (viscosity <= 0.0) {
	Tcl_AppendResult(interp, "viscosity must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.viscosity = viscosity;

    mpi_bcast_lb_params(LBPAR_VISCOSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_bulk_visc(Tcl_Interp *interp, int argc, char *argv[], int *change) {
  double bulk_visc;

  if (argc < 1) {
    Tcl_AppendResult(interp, "bulk_viscosity requires 1 argument", (char *)NULL);
    return TCL_ERROR;
  }
  if (!ARG0_IS_D(bulk_visc)) {
    Tcl_AppendResult(interp, "wrong argument for bulk_viscosity", (char *)NULL);
    return TCL_ERROR;
  }
  if (bulk_visc < 0.0) {
    Tcl_AppendResult(interp, "bulk_viscosity must be positive", (char *)NULL);
    return TCL_ERROR;
  }

  *change =1;
  lbpar.bulk_viscosity = bulk_visc;

  mpi_bcast_lb_params(LBPAR_BULKVISC);

  return TCL_OK;

}

static int lbfluid_parse_friction(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double friction;

    if (argc < 1) {
	Tcl_AppendResult(interp, "friction requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(friction)) {
	Tcl_AppendResult(interp, "wrong argument for friction", (char *)NULL);
	return TCL_ERROR;
    }
    if (friction <= 0.0) {
	Tcl_AppendResult(interp, "friction must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.friction = friction;

    mpi_bcast_lb_params(LBPAR_FRICTION);
 
    return TCL_OK;
}

static int lbfluid_parse_ext_force(Tcl_Interp *interp, int argc, char *argv[], int *change) {
#ifdef EXTERNAL_FORCES
    double ext_f[3];
    if (argc < 3) {
	Tcl_AppendResult(interp, "ext_force requires 3 arguments", (char *)NULL);
	return TCL_ERROR;
    }
    else {
 	if (!ARG_IS_D(0, ext_f[0])) return TCL_ERROR;
	if (!ARG_IS_D(1, ext_f[1])) return TCL_ERROR;
	if (!ARG_IS_D(2, ext_f[2])) return TCL_ERROR;
    }
    
    *change = 3;

    /* external force is stored in lattice units */
    lbpar.ext_force[0] = ext_f[0]*agrid*agrid*tau*tau;
    lbpar.ext_force[1] = ext_f[1]*agrid*agrid*tau*tau;
    lbpar.ext_force[2] = ext_f[2]*agrid*agrid*tau*tau;
    
    mpi_bcast_lb_params(LBPAR_EXTFORCE);
 
    return TCL_OK;
#else
  Tcl_AppendResult(interp, "EXTERNAL_FORCES not compiled in!", NULL);
  return TCL_ERROR;
#endif
}
#endif /* LB */

/** Parser for the \ref lbnode command. */
int lbnode_cmd(ClientData data, Tcl_Interp *interp, int argc, char **argv) {
#ifdef LB
   int err=TCL_ERROR;
   int coord[3];

   --argc; ++argv;
   
   if (argc < 3) {
     Tcl_AppendResult(interp, "too few arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   }

   if (!ARG_IS_I(0,coord[0]) || !ARG_IS_I(1,coord[1]) || !ARG_IS_I(2,coord[2])) {
     Tcl_AppendResult(interp, "wrong arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   } else {
     argc-=3; argv+=3;

     if (ARG0_IS_S("print"))
       err = lbnode_parse_print(interp, argc-1, argv+1, coord);
     else {
       Tcl_AppendResult(interp, "unknown feature \"", argv[0], "\" of lbnode", (char *)NULL);
       err = TCL_ERROR;
     }
     
   }
     
   return err;
#else /* !defined LB */
  Tcl_AppendResult(interp, "LB is not compiled in!", NULL);
  return TCL_ERROR;
#endif
}

/** Parser for the \ref lbfluid command. */
int lbfluid_cmd(ClientData data, Tcl_Interp *interp, int argc, char **argv) {
#ifdef LB
  int err = TCL_OK;
  int change = 0;
  
  argc--; argv++;

  if (argc < 1) {
      Tcl_AppendResult(interp, "too few arguments to \"lbfluid\"", (char *)NULL);
      err = TCL_ERROR;
  }
  else if (ARG0_IS_S("off")) {
    err = TCL_ERROR;
  }
  else if (ARG0_IS_S("init")) {
    err = TCL_ERROR;
  }
  else while (argc > 0) {
      if (ARG0_IS_S("grid") || ARG0_IS_S("agrid"))
	  err = lbfluid_parse_agrid(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("tau"))
	  err = lbfluid_parse_tau(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("density"))
	  err = lbfluid_parse_density(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("viscosity"))
	  err = lbfluid_parse_viscosity(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("bulk_viscosity"))
	  err = lbfluid_parse_bulk_visc(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("friction") || ARG0_IS_S("coupling"))
	  err = lbfluid_parse_friction(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("ext_force"))
	  err = lbfluid_parse_ext_force(interp, argc-1, argv+1, &change);
      else {
	  Tcl_AppendResult(interp, "unknown feature \"", argv[0],"\" of lbfluid", (char *)NULL);
	  err = TCL_ERROR ;
      }

      if ((err = mpi_gather_runtime_errors(interp, err))) break;

      argc -= (change + 1);
      argv += (change + 1);
  }

  lattice_switch = (lattice_switch | LATTICE_LB) ;
  mpi_bcast_parameter(FIELD_LATTICE_SWITCH) ;

  /* thermo_switch is retained for backwards compatibility */
  thermo_switch = (thermo_switch | THERMO_LB);
  mpi_bcast_parameter(FIELD_THERMO_SWITCH);

  return err;    
#else /* !defined LB */
  Tcl_AppendResult(interp, "LB is not compiled in!", NULL);
  return TCL_ERROR;
#endif
}

/*@}*/
