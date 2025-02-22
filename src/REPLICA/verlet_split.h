/* -*- c++ -*- -------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef INTEGRATE_CLASS
// clang-format off
IntegrateStyle(verlet/split,VerletSplit);
// clang-format on
#else

#ifndef LMP_VERLET_SPLIT_H
#define LMP_VERLET_SPLIT_H

#include "verlet.h"

namespace LAMMPS_NS {

class VerletSplit : public Verlet {
 public:
  VerletSplit(class LAMMPS *, int, char **);
  ~VerletSplit() override;
  void init() override;
  void setup(int) override;
  void setup_minimal(int) override;
  void run(int) override;
  double memory_usage() override;

 private:
  int master;                            // 1 if an Rspace proc, 0 if Kspace
  int me_block;                          // proc ID within Rspace/Kspace block
  int ratio;                             // ratio of Rspace procs to Kspace procs
  int *qsize, *qdisp, *xsize, *xdisp;    // MPI gather/scatter params for block comm
  MPI_Comm block;                        // communicator within one block
  
  int tip4pflag;                         // 1 if Kspace method sets tip4pflag

  double **f_kspace;    // copy of Kspace forces on Rspace procs
  int maxatom;

  void rk_setup();
  void r2k_comm();
  void k2r_comm();
};

}    // namespace LAMMPS_NS

#endif
#endif
