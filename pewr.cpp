
#define _USE_MATH_DEFINES
#include <cmath>

#include <complex>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdint.h>
#include <signal.h>

#ifdef _WIN32
//needed for chdir on windows
#include <direct.h>
#endif

#ifndef SINGLE_THREAD
#include <omp.h>
#endif

#include <fftw3.h>

#define NDEBUG
#include "Array.h"
#include "time.h"

using namespace std;
using namespace Array;

#ifndef USE_FLOATS
//computations should be done with doubles, higher accuracy
typedef double Real;

class FFTWreal {
	fftw_plan plan;
public:
	FFTWreal(int n0, int n1, void *in, void *out, int sign, unsigned int flags){
		plan = fftw_plan_dft_2d(n0, n1, reinterpret_cast<fftw_complex*>(in), reinterpret_cast<fftw_complex*>(out), sign, flags);
	}
	~FFTWreal(){ fftw_destroy_plan(plan); }
	void operator()(){ fftw_execute(plan); }
};
#else
//computations should be done with floats, higher speed
typedef float Real;
class FFTWreal {
	fftwf_plan plan;
public:
	FFTWreal(int n0, int n1, void *in, void *out, int sign, unsigned int flags){
		plan = fftwf_plan_dft_2d(n0, n1, reinterpret_cast<fftwf_complex*>(in), reinterpret_cast<fftwf_complex*>(out), sign, flags);
	}
	~FFTWreal(){ fftwf_destroy_plan(plan); }
	void operator()(){ fftwf_execute(plan); }
};
#endif

typedef std::complex<Real> Complex;

typedef array2<Complex> ArrayComplex;
typedef array2<Real>    ArrayReal;
typedef array2<bool>    ArrayBool;

template <class T> std::string to_str(T a){
	std::stringstream out;
	out << a;
	return out.str();
}

bool interrupted = false;

void interrupt(int sig){
	if(interrupted){
		cout << "Second interrupt, exiting ungracefully\n";
		exit(1);
	}
	cout << "Interrupt, finishing iteration and outputting ...";
	cout.flush();
	interrupted = true;
}

void die(int code, const string & str){
	cout << str << endl;
	exit(code);
}

struct Plane {
	int size, padding;
	Real         fval;      // focal plane
	ArrayReal    amplitude; // amplitudes, stores the initial image until converted to amplitudes
	ArrayComplex prop;      // propagation value to defocus the exit wave
	ArrayComplex ew;        // exit wave plane in the real domain
	FFTWreal     fftfwd;    // fast fourier transform in the forward direction space -> freq
	FFTWreal     fftbwd;    // fast fourier transform in the reverse direction freq -> space

	Plane(int _size, int _padding) :
		size(_size), padding(_padding),
		amplitude(size, size, 16),
		prop(     padding, padding, 16),
		ew(       padding, padding, 16),
		fftfwd(padding, padding, ew(), ew(), FFTW_FORWARD, FFTW_MEASURE),
		fftbwd(padding, padding, ew(), ew(), FFTW_BACKWARD, FFTW_MEASURE) {
	}

	template <class T> void import(const string & name){
		ifstream ifs(name.c_str(), ios::in | ios::binary);
		T in;
		for(int x = 0; x < size; x++){
			for(int y = 0; y < size; y++){
				ifs.read( (char *) & in, sizeof(T));
				amplitude[x][y] = in;
			}
		}
		ifs.close();
	}

	void dump(const string & name){
		ofstream ofs(name.c_str(), ios::out | ios::binary);
		for(int x = 0; x < padding; x++)
			for(int y = 0; y < padding; y++)
				ofs.write((const char*)& ew[x][y], sizeof(Complex));
		ofs.close();
	}

	double mean(){
		double sum = 0;
		for(int x = 0; x < size; x++)
			for(int y = 0; y < size; y++)
				sum += amplitude[x][y];
		return sum/(size*size);
	}

	void compute_amplitudes(){
		for(int x = 0; x < size; x++)
			for(int y = 0; y < size; y++)
				amplitude[x][y] = sqrt(abs(amplitude[x][y]));
	}
};

class PEWR {
	bool            verbose; // output the timings of each part of the algorithm
	int             size;    // size of the planes without padding
	int             padding; // size of the planes with padding
	int             nplanes; // number of planes
	int             iters;   // number of iterations
	double          lambda;  // wavelength of electrons
	double          psize;   // pixel size
	double          qmax;    // aperature size for the top hat filter
	vector<Plane *> planes;  // stack of planes
	ArrayBool       tophat;  // precompute q2 boundary
	ArrayComplex    ew;      // current best guess of the exit wave in space domain
	ArrayComplex    ewfft;   // current best guess of the exit wave in frequency domain
	string          output;  // prefix for the name of the output file
	int             outputfreq;  // output on a linear scale, ie if x=10, output 10, 20, 30, 40, 50, etc
	double          outputgeom;  // output on a geometric scale, ie if x=2, output 2, 4, 8, 16, 32, etc
	int             outputlast;  // output the last few iterations

	double q2(int x, int y){
		double qx = (((x + padding/2) % padding) - padding/2) / ( padding * psize);
		double qy = (((y + padding/2) % padding) - padding/2) / ( padding * psize);
		return qx*qx + qy*qy;
	}

public:
	PEWR(const string & stackHDFfile, const string & config){
		verbose = false;
		size    = 0;
		padding = 0;
		nplanes = 0;
		qmax    = 0;
		lambda  = 0;
		psize   = 0;
		iters   = 0;
		outputfreq = 0;
		outputgeom = 0;
		outputlast = 1;

		string type, guesstype;
		int startiter = 0;

		ifstream ifs(config.c_str(), ifstream::in);

		string::size_type dirpos = config.find("/");
		if(dirpos != string::npos){
			string dir = config.substr(0, dirpos);
			if(chdir(dir.c_str()) == -1)
				die(1, "Cannot change directories to " + dir);
		}

		Time start;
		cout << "Parsing config file, loading data ... ";
		cout.flush();

		bool setfvals = false, setplanes = false;

		while(ifs.good()){
			string cmd;
			ifs >> cmd;

			if(cmd == "")
				break;

//			cout << cmd << endl;

			if(cmd == "size"){
				ifs >> size;
			}else if(cmd == "padding"){
				ifs >> padding;

				ew.Allocate(   padding, padding, 16);
				ewfft.Allocate(padding, padding, 16);
			}else if(cmd == "verbose"){
				verbose = true;
			}else if(cmd == "threads"){
				int numthreads;
				ifs >> numthreads;
#ifndef SINGLE_THREAD
				omp_set_num_threads(numthreads);
#endif
			}else if(cmd == "nplanes"){
				if(size == 0 || padding == 0)
					die(1, "padding and size must be set before nplanes");

				ifs >> nplanes;
				for(int i = 0; i < nplanes; i++)
					planes.push_back(new Plane(size, padding));
			}else if(cmd == "qmax"){
				ifs >> qmax;
			}else if(cmd == "lambda"){
				ifs >> lambda;
			}else if(cmd == "psize"){
				ifs >> psize;
			}else if(cmd == "iters"){
				ifs >> iters;
			}else if(cmd == "type"){
				ifs >> type;
			}else if(cmd == "output"){
				ifs >> output;
			}else if(cmd == "outputfreq"){
				ifs >> outputfreq;
			}else if(cmd == "outputgeom"){
				ifs >> outputgeom;
			}else if(cmd == "outputlast"){
				ifs >> outputlast;
			}else if(cmd == "planes"){
				if(nplanes == 0)
					die(1, "nplanes size must be set before planes");

				if(type == "")
					die(1, "type must be set before planes");

				for(int i = 0; i < nplanes; i++){
					string name;
					ifs >> name;

					if(     type == "uint8" ) planes[i]->import<uint8_t>(name);
					else if(type ==  "int8" ) planes[i]->import< int8_t>(name);
					else if(type == "uint16") planes[i]->import<uint16_t>(name);
					else if(type ==  "int16") planes[i]->import< int16_t>(name);
					else if(type == "uint32") planes[i]->import<uint32_t>(name);
					else if(type ==  "int32") planes[i]->import< int32_t>(name);
					else if(type == "float" ) planes[i]->import<float>(name);
					else if(type == "double") planes[i]->import<double>(name);
					else die(1, "Unknown type " + type);
				}
				setplanes = true;
			}else if(cmd == "fvals"){
				if(nplanes == 0)
					die(1, "nplanes size must be set before fvals");

				for(int i = 0; i < nplanes; i++)
					ifs >> planes[i]->fval;
				setfvals = true;
			}else if(cmd == "frange"){
				if(nplanes == 0)
					die(1, "nplanes size must be set before frange");

				double start, incr;
				ifs >> start >> incr;

				for(int i = 0; i < nplanes; i++){
					planes[i]->fval = start;
					start += incr;
				}
				setfvals = true;
			}else if(cmd == "guesstype"){
				ifs >> guesstype;
				if(startiter)
					die(1, "guesstype must come before guess");
			}else if(cmd == "guess"){
				string name;
				ifs >> name >> startiter;

				ifstream ifguess(name.c_str(), ios::in | ios::binary);
				if(guesstype == ""){
					for(int x = 0; x < padding; x++)
						for(int y = 0; y < padding; y++)
							ifguess.read( (char *) & ew[x][y], sizeof(Complex));
				}else if(guesstype == "float"){
					for(int x = 0; x < padding; x++){
						for(int y = 0; y < padding; y++){
							complex<float> temp;
							ifguess.read( (char *) & temp, sizeof(complex<float>));
							ew[x][y] = temp;
						}
					}
				}else if(guesstype == "double"){
					for(int x = 0; x < padding; x++){
						for(int y = 0; y < padding; y++){
							complex<double> temp;
							ifguess.read( (char *) & temp, sizeof(complex<double>));
							ew[x][y] = temp;
						}
					}
				}else{
					die(1, "unknown guesstype, choose double or float");
				}
				ifguess.close();
			}else{
				die(1, "Unknown command " + cmd);
			}
		}

		ifs.close();

		if(size    == 0) die(1, "size must be defined in the config file");
		if(padding == 0) die(1, "padding must be defined in the config file");
		if(lambda  == 0) die(1, "lambda (ie wavelength) must be defined in the config file");
		if(qmax    == 0) die(1, "qmax must be defined in the config file");
		if(psize   == 0) die(1, "psize must be defined in the config file");
		if(iters   == 0) die(1, "iters must be defined in the config file");
		if(nplanes == 0) die(1, "nplanes must be defined in the config file");
		if(!setfvals)    die(1, "must set either fvals or frange in the config file");
		if(!setplanes)   die(1, "must set the planes in the config file");
		if(output == "") die(1, "No output prefix is defined in the config file");

		cout << "precomputing data ... ";
		cout.flush();

		//normalize 
		double mean = 0;
		#pragma omp parallel for schedule(guided) reduction(+:mean)
		for(int i = 0; i < nplanes; i++)
			mean += planes[i]->mean();
		mean /= nplanes;
		#pragma omp parallel for schedule(guided)
		for(int i = 0; i < nplanes; i++)
			planes[i]->amplitude *= 1.0/mean;

		//compute amplitudes
		#pragma omp parallel for schedule(guided)
		for(int i = 0; i < nplanes; i++)
			planes[i]->compute_amplitudes();

		//precompute q2
		double qmax2 = qmax*qmax;
		tophat.Allocate(padding, padding, 16);
		#pragma omp parallel for schedule(guided)
		for(int x = 0; x < padding; x++)
			for(int y = 0; y < padding; y++)
				tophat[x][y] = (q2(x, y) <= qmax2);

		//compute propagation arrays
		#pragma omp parallel for schedule(guided)
		for(int i = 0; i < nplanes; i++){
			for(int x = 0; x < padding; x++){
				for(int y = 0; y < padding; y++){
					double chi = M_PI * lambda * planes[i]->fval * q2(x, y);
					planes[i]->prop[x][y] = polar(1.0, -chi);
				}
			}
		}

		//setup the base fftw plans
		FFTWreal fftfwd(padding, padding, ew(), ewfft(), FFTW_FORWARD, FFTW_MEASURE);
		FFTWreal fftbwd(padding, padding, ewfft(), ew(), FFTW_BACKWARD, FFTW_MEASURE);

		//setup the initial approximations
		if(startiter == 0)
			#pragma omp parallel for schedule(guided)
			for(int x = 0; x < padding; x++)
				for(int y = 0; y < padding; y++)
					ew[x][y] = Complex(1, 0);

		fftfwd();

		cout << "done in " << (int)((Time() - start)*1000) << " msec\n";
		cout.flush();

		double nextgeomoutput = 1;
		if(outputgeom > 0)
			while(nextgeomoutput <= startiter)
				nextgeomoutput *= outputgeom;

		// Run iterations
		for(int iter = startiter+1; iter <= iters && !interrupted; iter++){

			Time startiter;
			cout << "Iter " << iter << " ...";
			cout.flush();

			double timedelta[7];
			for(int i = 0; i < 7; i++)
				timedelta[i] = 0;

			#pragma omp parallel for schedule(dynamic)
			for(int p = 0; p < nplanes; p++){
				Plane * plane = planes[p];

				LapTime laptime;

				// Propagate EW to each plane
				for(int x = 0; x < padding; x++){
					for(int y = 0; y < padding; y++){
						if(tophat[x][y]){
							plane->ew[x][y] = ewfft[x][y]*plane->prop[x][y];
						}else{
							plane->ew[x][y] = 0;
						}
					}
				}

				timedelta[0] += laptime();

				plane->fftbwd(); //plane->ewfft => plane->ew

				timedelta[1] += laptime();

				plane->ew *= 1.0/(padding*padding);

				timedelta[2] += laptime();

				// Replace EW amplitudes
				for(int x = 0; x < size; x++)
					for(int y = 0; y < size; y++)
						plane->ew[x][y] = polar(plane->amplitude[x][y], arg(plane->ew[x][y]));

				timedelta[3] += laptime();

				// Back propagate EW to zero plane, backpropagation is merged with mean
				plane->fftfwd(); //plane->ew => plane->ewfft

				timedelta[4] += laptime();
			}

			LapTime laptime;

			// Backpropagate and find mean EW
			#pragma omp parallel for schedule(guided)
			for(int x = 0; x < padding; x++){
				for(int y = 0; y < padding; y++){
					if(tophat[x][y]){
						Complex mean = 0;
						for(int p = 0; p < nplanes; p++)
							mean += planes[p]->ew[x][y] * conj(planes[p]->prop[x][y]);
						ewfft[x][y] = mean / (Real)nplanes;
					}else{
						ewfft[x][y] = 0;
					}
				}
			}

			timedelta[5] += laptime();

			//output exit wave
			if((outputfreq > 0 && iter % outputfreq == 0) ||
			   (outputgeom > 0 && nextgeomoutput <= iter) ||
			   (iters - iter < outputlast) ||
			   interrupted){

				if(outputgeom > 0)
					while(nextgeomoutput <= iter)
						nextgeomoutput *= outputgeom;

				fftbwd(); //ewfft -> ew

				ew *= 1.0/(padding*padding);

				ofstream ofs((output + "." + to_str(iter)).c_str(), ios::out | ios::binary);
				for(int x = 0; x < padding; x++)
					for(int y = 0; y < padding; y++)
						ofs.write((const char*)& ew[x][y], sizeof(Complex));
				ofs.close();
			}

			timedelta[6] += laptime();

			if(verbose)
				for(int i = 0; i < 7; i++)
					cout << " " << (int)(timedelta[i]*1000);

			cout << " done in " << (int)((Time() - startiter)*1000) << " msec\n";
		}

		cout << "Completed in " << (int)(Time() - start) << " sec\n";
	}
};


int main(int argc, char **argv){
	//PEWR pewr(argv[1]);

	// Check that stack of micrographs was passed in
	if(argc < 2)
		die(1, "Must pass an HDF image stack as first argument.");
	// Loop through input arguments to make sure a config file was specified with -c	
	string configFile = "";
	for(int i = 2; i < argc - 1; i++){
		if(strcmp(argv[i],"-c") == 0)
			configFile = argv[i+1];
	}

	signal(SIGINT,  interrupt);
	signal(SIGTERM, interrupt);

	PEWR pewr(argv[1],configFile);
}


#define NDEBUG
#include "Array.h"
#include "time.h"

using namespace std;
using namespace Array;

