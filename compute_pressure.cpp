/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "compute_pressure.h"

#include "atom.h"
#include "update.h"
#include "domain.h"
#include "modify.h"
#include "fix.h"
#include "fix_store.h"
#include "force.h"
#include "pair.h"
#include "pair_hybrid.h"
#include "bond.h"
#include "angle.h"
#include "dihedral.h"
#include "improper.h"
#include "kspace.h"
#include "error.h"
#include "memory.h"
#include "compute.h"
#include "group.h"
#include "compute_chunk_atom.h"

#include <cctype>
#include <cstring>
using namespace LAMMPS_NS;
using namespace std;

/* ---------------------------------------------------------------------- */

ComputePressure::ComputePressure(LAMMPS *lmp, int narg, char **arg) :
  Compute(lmp, narg, arg),
  vptr(NULL), id_temp(NULL)
{
  if (narg < 4) error->all(FLERR,"Illegal compute pressure command");
  if (igroup) error->all(FLERR,"Compute pressure must use group all");

  scalar_flag = vector_flag = 1;
  size_vector = 6;
  extscalar = 0;
  extvector = 0;
  pressflag = 1;
  timeflag = 1;
  comgroup = NULL;
  // store temperature ID used by pressure computation
  // insure it is valid for temperature computation

  if (strcmp(arg[3],"NULL") == 0) id_temp = NULL;
  else {
    int n = strlen(arg[3]) + 1;
    id_temp = new char[n];
    strcpy(id_temp,arg[3]);

    int icompute = modify->find_compute(id_temp);
    if (icompute < 0)
      error->all(FLERR,"Could not find compute pressure temperature ID");
    if (modify->compute[icompute]->tempflag == 0)
      error->all(FLERR,"Compute pressure temperature ID does not "
                 "compute temperature");
  }

  // process optional args

  pairhybridflag = 0;
  if (narg == 4) {
    keflag = 1;
    pairflag = 1;
    bondflag = angleflag = dihedralflag = improperflag = 1;
    kspaceflag = fixflag = 1;
  } else {
    keflag = 0;
    pairflag = 0;
    bondflag = angleflag = dihedralflag = improperflag = 0;
    kspaceflag = fixflag = 0;
    int iarg = 4;
    while (iarg < narg) {
      if (strcmp(arg[iarg],"ke") == 0) keflag = 1;
      else if (strcmp(arg[iarg],"pair/hybrid") == 0) {
        int n = strlen(arg[++iarg]) + 1;
        if (lmp->suffix) n += strlen(lmp->suffix) + 1;
        pstyle = new char[n];
        strcpy(pstyle,arg[iarg++]);

        nsub = 0;

        if (narg > iarg) {
          if (isdigit(arg[iarg][0])) {
            nsub = force->inumeric(FLERR,arg[iarg]);
            ++iarg;
            if (nsub <= 0)
              error->all(FLERR,"Illegal compute pressure command");
          }
        }

        // check if pair style with and without suffix exists

        pairhybrid = (Pair *) force->pair_match(pstyle,1,nsub);
        if (!pairhybrid && lmp->suffix) {
          strcat(pstyle,"/");
          strcat(pstyle,lmp->suffix);
          pairhybrid = (Pair *) force->pair_match(pstyle,1,nsub);
        }

        if (!pairhybrid)
          error->all(FLERR,"Unrecognized pair style in compute pressure command");

        pairhybridflag = 1;
      }
      else if (strcmp(arg[iarg],"pair") == 0) pairflag = 1;
      else if (strcmp(arg[iarg],"bond") == 0) bondflag = 1;
      else if (strcmp(arg[iarg],"angle") == 0) angleflag = 1;
      else if (strcmp(arg[iarg],"dihedral") == 0) dihedralflag = 1;
      else if (strcmp(arg[iarg],"improper") == 0) improperflag = 1;
      else if (strcmp(arg[iarg],"kspace") == 0) kspaceflag = 1;
      else if (strcmp(arg[iarg],"fix") == 0) fixflag = 1;
      else if (strcmp(arg[iarg],"virial") == 0) {
        pairflag = 1;
        bondflag = angleflag = dihedralflag = improperflag = 1;
        kspaceflag = fixflag = 1;
      }
	     else if (strcmp(arg[iarg],"com") == 0) {
        	int n = strlen(arg[++iarg]) + 1;
        	if (lmp->suffix) n += strlen(lmp->suffix) + 1;
        	comgroup = new char[n];
        	strcpy(comgroup,arg[iarg++]);
      }	
       else error->all(FLERR,"Illegal compute pressure command");
      iarg++;
    }
  }
  // error check

  if (keflag && id_temp == NULL)
    error->all(FLERR,"Compute pressure requires temperature ID "
               "to include kinetic energy");

  vector = new double[size_vector];
  nvirial = 0;
  vptr = NULL;
}

/* ---------------------------------------------------------------------- */

ComputePressure::~ComputePressure()
{
  delete [] id_temp;
  delete [] vector;
  delete [] vptr;
}

/* ---------------------------------------------------------------------- */

void ComputePressure::init()
{
  boltz = force->boltz;
  nktv2p = force->nktv2p;
  dimension = domain->dimension;

  // set temperature compute, must be done in init()
  // fixes could have changed or compute_modify could have changed it

  if (keflag) {
    int icompute = modify->find_compute(id_temp);
    if (icompute < 0)
      error->all(FLERR,"Could not find compute pressure temperature ID");
    temperature = modify->compute[icompute];
  }

  // recheck if pair style with and without suffix exists

  if (pairhybridflag) {
    pairhybrid = (Pair *) force->pair_match(pstyle,1,nsub);
    if (!pairhybrid && lmp->suffix) {
      strcat(pstyle,"/");
      strcat(pstyle,lmp->suffix);
      pairhybrid = (Pair *) force->pair_match(pstyle,1,nsub);
    }

    if (!pairhybrid)
      error->all(FLERR,"Unrecognized pair style in compute pressure command");
  }

  // detect contributions to virial
  // vptr points to all virial[6] contributions

  delete [] vptr;
  nvirial = 0;
  vptr = NULL;

  if (pairhybridflag && force->pair) nvirial++;
  if (pairflag && force->pair) nvirial++;
  if (bondflag && atom->molecular && force->bond) nvirial++;
  if (angleflag && atom->molecular && force->angle) nvirial++;
  if (dihedralflag && atom->molecular && force->dihedral) nvirial++;
  if (improperflag && atom->molecular && force->improper) nvirial++;
  if (fixflag)
    for (int i = 0; i < modify->nfix; i++)
      if (modify->fix[i]->thermo_virial) nvirial++;

  if (nvirial) {
    vptr = new double*[nvirial];
    nvirial = 0;
    if (pairhybridflag && force->pair) {
      PairHybrid *ph = (PairHybrid *) force->pair;
      ph->no_virial_fdotr_compute = 1;
      vptr[nvirial++] = pairhybrid->virial;
    }
    if (pairflag && force->pair) vptr[nvirial++] = force->pair->virial;
    if (bondflag && force->bond) vptr[nvirial++] = force->bond->virial;
    if (angleflag && force->angle) vptr[nvirial++] = force->angle->virial;
    if (dihedralflag && force->dihedral)
      vptr[nvirial++] = force->dihedral->virial;
    if (improperflag && force->improper)
      vptr[nvirial++] = force->improper->virial;
    if (fixflag)
      for (int i = 0; i < modify->nfix; i++)
        if (modify->fix[i]->thermo_virial)
          vptr[nvirial++] = modify->fix[i]->virial;
  }

  // flag Kspace contribution separately, since not summed across procs

  if (kspaceflag && force->kspace) kspace_virial = force->kspace->virial;
  else kspace_virial = NULL;
}

/* ----------------------------------------------------------------------
   compute total pressure, averaged over Pxx, Pyy, Pzz
------------------------------------------------------------------------- */

double ComputePressure::compute_scalar()
{
  invoked_scalar = update->ntimestep;
  if (update->vflag_global != invoked_scalar)
    error->all(FLERR,"Virial was not tallied on needed timestep");

  // invoke temperature if it hasn't been already

  double t;
  if (keflag) {
    if (temperature->invoked_scalar != update->ntimestep)
      t = temperature->compute_scalar();
    else t = temperature->scalar;
  }

  if (dimension == 3) {
    inv_volume = 1.0 / (domain->xprd * domain->yprd * domain->zprd);
    virial_compute(3,3);
    if (keflag)
      scalar = (temperature->dof * boltz * t +
                virial[0] + virial[1] + virial[2]) / 3.0 * inv_volume * nktv2p;
    else
      scalar = (virial[0] + virial[1] + virial[2]) / 3.0 * inv_volume * nktv2p;
  } else {
    inv_volume = 1.0 / (domain->xprd * domain->yprd);
    virial_compute(2,2);
    if (keflag)
      scalar = (temperature->dof * boltz * t +
                virial[0] + virial[1]) / 2.0 * inv_volume * nktv2p;
    else
      scalar = (virial[0] + virial[1]) / 2.0 * inv_volume * nktv2p;
  }

  //COM calculation block
  //invoked only on presence of com keyword

 if (comgroup != NULL)
 {
  int icompute = modify->find_compute(comgroup);
  if (icompute < 0)
    error->all(FLERR,"Chunk/atom compute does not exist for compute_pressure center-of-mass calculations");
  comchunk = (ComputeChunkAtom *) modify->compute[icompute];

  nchunk = comchunk->setup_chunks();
  comchunk->compute_ichunk();
  int *comichunk = nullptr;
  memory->create(comichunk,atom->nmax,"compute_pressure:comichunk");
  comichunk = comchunk->ichunk;
 
  maxchunk = 0;
  firstflag = massneed = 1;
  if (nchunk > maxchunk){
      maxchunk = nchunk;
      memory->create(massproc,maxchunk,"compute_pressure:massproc");
      memory->create(masstotal,maxchunk,"compute_pressure:masstotal");
      memory->create(com,maxchunk,3,"compute_pressure:com");
      memory->create(comall,maxchunk,3,"compute_pressure:comall");
    }

    for (int i = 0; i < nchunk; i++){
      com[i][0] = com[i][1] = com[i][2] = 0.0;
    }
    
  if (massneed){
    for (int i = 0; i < nchunk; i++) massproc[i] = 0.0;
  }
 
  double **x = atom->x;
  int *mask = atom->mask;
  int *type = atom->type;
  imageint *image = atom->image;
  double *mass = atom->mass;
  double *rmass = atom->rmass;
  int nlocal = atom->nlocal;
  int index;
  double massone;
  double unwrap[3];
  
  int ngrp = group->ngroup;          
  char **nnames = group->names;        
  int igroup = group->find(nnames[0]);
  int groupbit = group->bitmask[igroup];
 
  for (int i = 0; i < nlocal; i++){
  if (mask[i] & groupbit) {
    index = comichunk[i]-1;
    if (index < 0) continue;
    if (rmass) massone = rmass[i];
    else massone = mass[type[i]];
    domain->unmap(x[i],image[i],unwrap);
    com[index][0] += unwrap[0] * massone;
    com[index][1] += unwrap[1] * massone;
    com[index][2] += unwrap[2] * massone;
    if (massneed) massproc[index] += massone;
  }
  }
    
  MPI_Allreduce(&com[0][0],&comall[0][0],3*nchunk,MPI_DOUBLE,MPI_SUM,world);
  if (massneed)
        MPI_Allreduce(massproc,masstotal,nchunk,MPI_DOUBLE,MPI_SUM,world);
  for (int i = 0; i < nchunk; i++) {
    if (masstotal[i] > 0.0) {
      comall[i][0] /= masstotal[i];
      comall[i][1] /= masstotal[i];
      comall[i][2] /= masstotal[i];
    } else comall[i][0] = comall[i][1] = comall[i][2] = 0.0;
  }
 
  double **f = atom->f;
  double *cterm;
  double correction_term = 0.0;
  
  memory->create(cterm,maxchunk,"compute_pressure:cterm");
  for (int i = 0; i < nchunk; i++) cterm[i] = 0.0;
 
  for (int i = 0; i < nlocal; i++){
    if (mask[i] & groupbit) {
      index = comichunk[i]-1;
      if (index < 0) continue;
      domain->unmap(x[i],image[i],unwrap);
      cterm[index] += (f[i][0]*(unwrap[0]-comall[index][0])) + (f[i][1]*(unwrap[1]-comall[index][1])) + (f[i][2]*(unwrap[2]-comall[index][2]));
    }
  }
 
 for (int i = 0; i < nchunk; i++) correction_term += cterm[i];
 if (dimension == 3) {
    inv_volume = 1.0 / (domain->xprd * domain->yprd * domain->zprd);
    scalar += correction_term * inv_volume / 3;
  } else {
    inv_volume = 1.0 / (domain->xprd * domain->yprd);
    scalar += correction_term * inv_volume / 2;
  }
  delete [] massproc
  delete [] masstotal
  delete [] com
  delete [] comall
  delete [] cterm
 }
 
 return scalar;
}

/* ----------------------------------------------------------------------
   compute pressure tensor
   assume KE tensor has already been computed
------------------------------------------------------------------------- */

void ComputePressure::compute_vector()
{
  invoked_vector = update->ntimestep;
  if (update->vflag_global != invoked_vector)
    error->all(FLERR,"Virial was not tallied on needed timestep");

  if (force->kspace && kspace_virial && force->kspace->scalar_pressure_flag)
    error->all(FLERR,"Must use 'kspace_modify pressure/scalar no' for "
               "tensor components with kspace_style msm");

  // invoke temperature if it hasn't been already

  double *ke_tensor;
  if (keflag) {
    if (temperature->invoked_vector != update->ntimestep)
      temperature->compute_vector();
    ke_tensor = temperature->vector;
  }

  if (dimension == 3) {
    inv_volume = 1.0 / (domain->xprd * domain->yprd * domain->zprd);
    virial_compute(6,3);
    if (keflag) {
      for (int i = 0; i < 6; i++)
        vector[i] = (ke_tensor[i] + virial[i]) * inv_volume * nktv2p;
    } else
      for (int i = 0; i < 6; i++)
        vector[i] = virial[i] * inv_volume * nktv2p;
  } else {
    inv_volume = 1.0 / (domain->xprd * domain->yprd);
    virial_compute(4,2);
    if (keflag) {
      vector[0] = (ke_tensor[0] + virial[0]) * inv_volume * nktv2p;
      vector[1] = (ke_tensor[1] + virial[1]) * inv_volume * nktv2p;
      vector[3] = (ke_tensor[3] + virial[3]) * inv_volume * nktv2p;
      vector[2] = vector[4] = vector[5] = 0.0;
    } else {
      vector[0] = virial[0] * inv_volume * nktv2p;
      vector[1] = virial[1] * inv_volume * nktv2p;
      vector[3] = virial[3] * inv_volume * nktv2p;
      vector[2] = vector[4] = vector[5] = 0.0;
    }
  }
}

/* ---------------------------------------------------------------------- */

void ComputePressure::virial_compute(int n, int ndiag)
{
  int i,j;
  double v[6],*vcomponent;

  for (i = 0; i < n; i++) v[i] = 0.0;

  // sum contributions to virial from forces and fixes

  for (j = 0; j < nvirial; j++) {
    vcomponent = vptr[j];
    for (i = 0; i < n; i++) v[i] += vcomponent[i];
  }

  // sum virial across procs

  MPI_Allreduce(v,virial,n,MPI_DOUBLE,MPI_SUM,world);

  // KSpace virial contribution is already summed across procs

  if (kspace_virial)
    for (i = 0; i < n; i++) virial[i] += kspace_virial[i];

  // LJ long-range tail correction, only if pair contributions are included

  if (force->pair && pairflag && force->pair->tail_flag)
    for (i = 0; i < ndiag; i++) virial[i] += force->pair->ptail * inv_volume;
}

/* ---------------------------------------------------------------------- */

void ComputePressure::reset_extra_compute_fix(const char *id_new)
{
  delete [] id_temp;
  int n = strlen(id_new) + 1;
  id_temp = new char[n];
  strcpy(id_temp,id_new);
}
