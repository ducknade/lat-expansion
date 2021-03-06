#include <hash-cpp/crc32.h>

#include <iostream>
#include <fstream>

#include <sys/stat.h>
#include <unistd.h>

#include <qlat/config.h>
#include <qlat/utils.h>
#include <qlat/mpi.h>
#include <qlat/field.h>
#include <qlat/field-io.h>
#include <qlat/field-comm.h>
#include <qlat/field-rng.h>

#include "field-hmc.h"

// #include<qmp.h>
// #include<mpi.h>

#define PARALLEL_READING_THREADS 16

using namespace std;
using namespace cps;
using namespace qlat;

void naive_field_expansion(const cps::Lattice &lat, 
				qlat::Field<cps::Matrix> &gauge_field_qlat, 
				int mag)
{
	// from CPS to qlat
	sync_node();
#pragma omp parallel for
	for(long local_index = 0; local_index < GJP.VolNodeSites(); local_index++){
		int x_cps[4]; GJP.LocalIndex(local_index, x_cps);
		Coordinate x_qlat(mag * x_cps[0], mag * x_cps[1],
					mag * x_cps[2], mag * x_cps[3]);
                qlat::Vector<cps::Matrix> vec_qlat(gauge_field_qlat.get_elems(x_qlat));
                vec_qlat[0] = *lat.GetLink(x_cps, 0);
                vec_qlat[1] = *lat.GetLink(x_cps, 1);
                vec_qlat[2] = *lat.GetLink(x_cps, 2);
                vec_qlat[3] = *lat.GetLink(x_cps, 3);	
	}	
	sync_node();
	if(UniqueID() == 0) std::cout << "Field expansion finished." << std::endl;
}

void hmc_in_qlat(const Coordinate &totalSize, 
                        string config_addr, const Arg_chmc &arg,
			int argc, char *argv[]){
// 	Start(&argc, &argv);
// 	int totalSite[] = {totalSize[0], totalSize[1], totalSize[2], totalSize[3]};
// 	DoArg do_arg_coarse;
// 	setDoArg(do_arg_coarse, totalSite);
// 	GJP.Initialize(do_arg_coarse);
// 	LRG.Initialize();
// 
// 	// load config in CPS
// 	// load_config(config_addr.c_str());
// 
// 	// Lattice &lat = LatticeFactory::Create(F_CLASS_NONE, G_CLASS_NONE);
// 
// 	// Set up LQPS
// 
// 	begin(QMP_COMM_WORLD, Coordinate(SizeX(), SizeY(), SizeZ(), SizeT()));
// 	Coordinate totalSite_qlat(arg.mag * GJP.NodeSites(0) * SizeX(), 
// 				arg.mag * GJP.NodeSites(1) * SizeY(), 
// 				arg.mag * GJP.NodeSites(2) * SizeZ(), 
// 				arg.mag * GJP.NodeSites(3) * SizeT());
	begin(&argc, &argv);

	Geometry geoOrigin; geoOrigin.init(totalSize, DIM);
	Coordinate expansion(1, 1, 1, 1);
	geoOrigin.resize(expansion, expansion);

	Field<Matrix> gFieldOrigin;
	gFieldOrigin.init(geoOrigin);
	
	import_config_nersc(gFieldOrigin, config_addr, 16, true);
	fetch_expanded(gFieldOrigin);
 	report << "Coarse Configuration: " << config_addr << endl;
	report << "AVERAGE Plaquette Original = \t" << avg_plaquette(gFieldOrigin) << endl;

	Geometry geoExpanded;
	geoExpanded.init(arg.mag * totalSize, DIM);
	geoExpanded.resize(expansion, expansion);

	Field<Matrix> gFieldExpanded;
	gFieldExpanded.init(geoExpanded);

#pragma omp parallel for
	for(long index = 0; index < geoExpanded.local_volume(); index++){
		Coordinate x; geoExpanded.coordinate_from_index(x, index);
		for(int mu = 0; mu < geoExpanded.multiplicity; mu++){
			gFieldExpanded.get_elems(x)[mu].UnitMatrix();
	}}
	sync_node();
#pragma omp parallel for
	for(long index = 0; index < geoOrigin.local_volume(); index++){
		Coordinate x; geoOrigin.coordinate_from_index(x, index);
		Coordinate xExpanded = arg.mag * x;
		for(int mu = 0; mu < DIM; mu++){
			gFieldExpanded.get_elems(xExpanded)[mu] = 
						gFieldOrigin.get_elems_const(x)[mu]; 
	}}	
	sync_node();
	report << "Field Expansion Finished." << std::endl;
	
	Chart<Matrix> chart;
	produce_chart_envelope(chart, gFieldExpanded.geo, arg.gauge);
	
	fetch_expanded(gFieldExpanded);
	report << "AVERAGE Plaquette =     \t" << avg_plaquette(gFieldExpanded) << endl;
	report << "CONSTRAINED Plaquette = \t" 
		<< check_constrained_plaquette(gFieldExpanded, arg.mag) << endl;	

//  start hmc 
	FILE *pFile = fopen("/bgusr/home/jtu/mag/data/alg_gchmc_test.dat", "a");

	if(get_id_node() == 0){
		time_t now = time(NULL);
		fputs("# ", pFile);
		fputs(ctime(&now), pFile);
		fputs(show(gFieldExpanded.geo).c_str(), pFile); fputs("\n", pFile);
		fprintf(pFile, "# Coarse Config: %s\n", config_addr.c_str());
		fprintf(pFile, "# mag =        %i\n", arg.mag);
		fprintf(pFile, "# trajLength = %i\n", arg.trajectory_length);
		fprintf(pFile, "# numTraj =    %i\n", arg.num_trajectory);
		fprintf(pFile, "# beta =       %.3f\n", arg.beta);
		fprintf(pFile, "# dt =         %.5f\n", arg.dt);
		fprintf(pFile, 
			"# traj. number\texp(-DeltaH)\tavgPlaq\taccept/reject\n");
		fflush(pFile);
		report << "pFile opened." << endl;
	}

	run_chmc(gFieldExpanded, arg, pFile);
	
	fetch_expanded(gFieldExpanded);
	report << "AVERAGE Plaquette =     \t" << avg_plaquette(gFieldExpanded) << endl;
	report << "CONSTRAINED Plaquette = \t" 
		<< check_constrained_plaquette(gFieldExpanded, arg.mag) << endl;	
	
	Timer::display();

}

bool doesFileExist(const char *fn){
  struct stat sb;
  return 1 + stat(fn, &sb);
}

string str_printf(const char *format, ...){
        char cstr[512];
        va_list args; va_start(args, format);
	vsnprintf(cstr, sizeof(cstr), format, args);
        return string(cstr);
}

int main(int argc, char* argv[]){
	cout.precision(12);
	cout.setf(ios::showpoint);
	cout.setf(ios::showpos);
	cout.setf(ios::scientific);

	Coordinate total_size(24, 24, 24, 64);
	int mag = 2;
	
	int origin_start = 		380;
	int origin_end = 		680;
	int origin_interval = 	20; 

	string cps_config;
	string expanded_config;

	Arg_chmc arg;

	for(int i = origin_start; i <= origin_end; i += origin_interval){
		arg.mag = mag;
		arg.trajectory_length = 11;
		arg.num_trajectory = 1600;
		arg.beta = 5.40;
		arg.dt = 1. / arg.trajectory_length;
		arg.num_step_between_output = 10;
		arg.num_forced_accept_step = 20;
		arg.num_step_before_output = 60;

		Gauge gauge; gauge.type = qlat::WILSON;
		arg.gauge = gauge;

		cps_config = "/bgusr/data09/qcddata/DWF/2+1f/24nt64/IWASAKI+DSDR/"
		"b1.633/ls24/M1.8/ms0.0850/ml0.00107/evol1/configurations/"
		"ckpoint_lat." + show((long)i);

// 		expanded_config = "/bgusr/home/jtu/config/"
// 		"2+1f_24nt64_IWASAKI+DSDR_b1.633_ls24_M1.8_ms0.0850_ml0.00107/"
// 		"ckpoint_lat." + show((long)i) + "_mag" + show((long)mag_factor) + 
// 		"_b" + str_printf("%.2f", arg.beta) + "_WILSON/";

		expanded_config = "";

		mkdir(expanded_config.c_str(), 0777);	

		arg.export_dir_stem = expanded_config;
			
		hmc_in_qlat(total_size, cps_config, arg, argc, argv);
	}

	sync_node();
	cout << "[" << get_id_node() << "]: ""Program Ended Normally." << endl;

	return 0;
}

