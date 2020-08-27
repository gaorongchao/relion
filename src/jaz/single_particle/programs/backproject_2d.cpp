#include "backproject_2d.h"
#include <src/args.h>
#include <src/metadata_table.h>
#include <src/ctf.h>
#include <src/jaz/util/zio.h>
#include <src/jaz/util/log.h>
#include <src/jaz/single_particle/obs_model.h>
#include <src/jaz/image/interpolation.h>
#include <src/jaz/image/translation.h>
#include <src/jaz/image/tapering.h>
#include <src/jaz/image/normalization.h>
#include <src/jaz/image/resampling.h>
#include <src/jaz/util/image_file_helper.h>
#include <src/jaz/math/fft.h>
#include <src/jaz/single_particle/stack_helper.h>
#include <omp.h>


using namespace gravis;


void Backproject2D::read(int argc, char **argv)
{
	IOParser parser;

	try
	{
		IOParser parser;

		parser.setCommandLine(argc, argv);

		int general_section = parser.addSection("General options");

		particlesFn = parser.getOption("--i", "Input file (e.g. run_it023_data.star)", "");
		reextract = parser.checkOption("--reextract", "Extract particles from the micrographs");
		SNR = textToDouble(parser.getOption("--SNR", "Assumed signal-to-noise ratio", "0.1"));
		margin = textToDouble(parser.getOption("--m", "Margin around the particle [Px]", "20"));
		num_threads = textToInteger(parser.getOption("--j", "Number of OMP threads", "6"));
		outDir = parser.getOption("--o", "Output directory");

		Log::readParams(parser);

		if (parser.checkForErrors())
		{
			REPORT_ERROR("Errors encountered on the command line (see above), exiting...");
		}
	}
	catch (RelionError XE)
	{
		parser.writeUsage(std::cout);
		std::cerr << XE;
		exit(1);
	}
}

void Backproject2D::run()
{
	outDir = ZIO::makeOutputDir(outDir);

	ObservationModel obs_model;
	MetaDataTable particles_table;
	ObservationModel::loadSafely(particlesFn, obs_model, particles_table);

	int max_class = -1;

	for (long int p = 0; p < particles_table.numberOfObjects(); p++)
	{
		const int class_id = particles_table.getIntMinusOne(EMDL_PARTICLE_CLASS, p);

		if (class_id > max_class) max_class = class_id;
	}

	const int class_count = max_class + 1;

	if (class_count == 1)
	{
		Log::print("1 class found");
	}
	else
	{
		Log::print(ZIO::itoa(class_count)+" classes found");
	}


	std::vector<int> class_size(class_count, 0);

	for (long int p = 0; p < particles_table.numberOfObjects(); p++)
	{
		const int class_id = particles_table.getIntMinusOne(
					EMDL_PARTICLE_CLASS, p);

		class_size[class_id]++;
	}



	const int box_size = obs_model.getBoxSize(0);
	const double pixel_size = obs_model.getPixelSize(0);


	BufferedImage<double> average_stack(box_size, box_size, class_count);

	BufferedImage<dComplex> data(box_size / 2 + 1, box_size, num_threads * class_count);
	data.fill(dComplex(0.0, 0.0));

	BufferedImage<double> weight(box_size / 2 + 1, box_size, num_threads * class_count);
	weight.fill(0.0);


	std::vector<MetaDataTable> particles_by_micrograph = StackHelper::splitByMicrographName(particles_table);

	const int micrograph_count = particles_by_micrograph.size();

	for (int micrograph_id = 0; micrograph_id < micrograph_count; micrograph_id++)
	{
		Log::print("Micrograph " + ZIO::itoa(micrograph_id+1));

		const MetaDataTable& particles = particles_by_micrograph[micrograph_id];
		const int particle_count = particles.numberOfObjects();


		BufferedImage<float> micrograph;
		float mean_value = 0, std_dev = 1;
		double micrograph_pixel_size = pixel_size;
		double extraction_scale = 1;

		if (reextract)
		{
			const std::string micrograph_filename = particles.getString(EMDL_MICROGRAPH_NAME, 0);

			micrograph.read(micrograph_filename);
			mean_value = Normalization::computeMean(micrograph);
			std_dev = sqrt(Normalization::computeVariance(micrograph, mean_value));

			micrograph_pixel_size = ImageFileHelper::getSamplingRate(micrograph_filename);

			extraction_scale = pixel_size / micrograph_pixel_size;
		}

		const i2Vector micrograph_size(micrograph.xdim, micrograph.ydim);


		#pragma omp parallel for num_threads(num_threads)
		for (long int p = 0; p < particle_count; p++)
		{
			const int thread_id = omp_get_thread_num();
			const int class_id = particles.getIntMinusOne(EMDL_PARTICLE_CLASS, p);
			const int slice_id = thread_id * class_count + class_id;

			BufferedImage<float> particle_image_RS;
			BufferedImage<fComplex> particle_image_FS;

			const double dx_A = particles.getDouble(EMDL_ORIENT_ORIGIN_X_ANGSTROM, p);
			const double dy_A = particles.getDouble(EMDL_ORIENT_ORIGIN_Y_ANGSTROM, p);

			d2Vector shift;

			if (reextract)
			{
				const int extraction_box_size = extraction_scale * box_size;

				const d2Vector local_shift = d2Vector(dx_A, dy_A) / pixel_size;

				const d2Vector global_position_0(
					particles_table.getDouble(EMDL_IMAGE_COORD_X, p),
					particles_table.getDouble(EMDL_IMAGE_COORD_Y, p));

				const d2Vector global_position = global_position_0;// - local_shift;

				i2Vector integral_position(std::round(global_position.x), std::round(global_position.y));

				for (int dim = 0; dim < 2; dim++)
				{
					if (integral_position[dim] < extraction_box_size/2)
					{
						integral_position[dim] = extraction_box_size/2;
					}
					else if (integral_position[dim] > micrograph_size[dim] - extraction_box_size/2)
					{
						integral_position[dim] = micrograph_size[dim] - extraction_box_size/2;
					}

					shift[dim] = local_shift[dim];//micrograph_pixel_size * (integral_position[dim] - global_position[dim]);
				}

				BufferedImage<float> extraction_buffer(extraction_box_size, extraction_box_size);

				const int x0 = integral_position.x - extraction_box_size / 2;
				const int y0 = integral_position.y - extraction_box_size / 2;

				for (int y = 0; y < extraction_box_size; y++)
				for (int x = 0; x < extraction_box_size; x++)
				{
					//extraction_buffer(x,y) = -(micrograph(x0+x, y0+y) - mean_value) / std_dev;
					extraction_buffer(x,y) = -micrograph(x0+x, y0+y);
				}

				if (std::abs(micrograph_pixel_size - pixel_size) > 0.001)
				{
					particle_image_RS = Resampling::FourierCrop_fullStack(
								extraction_buffer, extraction_scale, num_threads, true);
				}
				else
				{
					particle_image_RS = extraction_buffer;
				}
			}
			else
			{
				shift = d2Vector(dx_A, dy_A) / pixel_size;

				std::string img_fn = particles_table.getString(EMDL_IMAGE_NAME, p);
				particle_image_RS.read(img_fn);
			}

			FFT::FourierTransform(particle_image_RS, particle_image_FS, FFT::Both);

			particle_image_FS(0,0) = 0;

			const double m = box_size / 2;
			Translation::shiftInFourierSpace2D(particle_image_FS, shift.x + m, shift.y + m);

			RawImage<dComplex> data_slice = data.getSliceRef(slice_id);
			RawImage<double> weight_slice = weight.getSliceRef(slice_id);

			backrotate_particle(
				particle_image_FS,
				p, particles_table,
				obs_model,
				data_slice,
				weight_slice);
		}
	}

	for (int class_id = 0; class_id < class_count; class_id++)
	{
		const int slice_id_0 = class_id;

		for (long int t = 1; t < num_threads; t++)
		{
			const int slice_id = t * class_count + class_id;

			data.getSliceRef(slice_id_0) += data.getSliceRef(slice_id);
			weight.getSliceRef(slice_id_0) += weight.getSliceRef(slice_id);
		}
	}

	for (int class_id = 0; class_id < class_count; class_id++)
	{
		RawImage<dComplex> data_slice = data.getSliceRef(class_id);
		RawImage<double> weight_slice = weight.getSliceRef(class_id);

		BufferedImage<double> average = reconstruct(
					data_slice, weight_slice, 1.0/SNR);

		const double radius = box_size/2;

		Tapering::taperCircularly2D(average, radius - margin, radius - margin + 5);

		average_stack.getSliceRef(class_id).copyFrom(average);
	}

	average_stack.write(outDir + "class_averages.mrc", pixel_size);
}

void Backproject2D::backrotate_particle(
		const RawImage<fComplex> image,
		long int particle_id,
		const MetaDataTable& particles_table,
		ObservationModel& obsModel,
		RawImage<dComplex>& data,
		RawImage<double>& weight)
{
	const int sh = data.xdim;
	const int s  = data.ydim;

	const double pixel_size = obsModel.getPixelSize(0);
	const double box_size_px = obsModel.getBoxSize(0);
	const double box_size_A = box_size_px * pixel_size;


	const double psi = DEG2RAD(particles_table.getDouble(EMDL_ORIENT_PSI, particle_id));

	const d2Matrix rot(
			 cos(psi), sin(psi),
			-sin(psi), cos(psi)	);

	CTF ctf;
	ctf.readByGroup(particles_table, &obsModel, particle_id);


	for (int y = 0; y < s;  y++)
	for (int x = 0; x < sh; x++)
	{
		const d2Vector p0(x, (y < s/2? y : y - s));
		const d2Vector p1 = rot * p0;

		const fComplex z = Interpolation::linearXY_complex_FftwHalf_wrap(
					image, p1.x, p1.y);

		const double c = ctf.getCTF(p1.x / box_size_A, p1.y / box_size_A);

		data(x,y)   += c * z;
		weight(x,y) += c * c;
	}
}


BufferedImage<double> Backproject2D::reconstruct(
		RawImage<dComplex>& data,
		RawImage<double>& weight,
		double Wiener_offset)
{
	const int sh = data.xdim;
	const int s  = data.ydim;

	BufferedImage<double> out_RS(s,s);
	BufferedImage<dComplex> out_FS(sh,s);

	for (int y = 0; y < s;  y++)
	for (int x = 0; x < sh; x++)
	{
		const double mod = (1 - 2 * (x % 2)) * (1 - 2 * (y % 2));

		out_FS(x,y) = mod * data(x,y) / (weight(x,y) + Wiener_offset);
	}

	FFT::inverseFourierTransform(out_FS, out_RS, FFT::Both);

	for (int y = 0; y < s; y++)
	for (int x = 0; x < s; x++)
	{
		const double xx = x - s/2;
		const double yy = y - s/2;

		if (xx == 0 && yy == 0)
		{
			// sinc at 0 is 1
		}
		else
		{
			const double r = sqrt(xx*xx + yy*yy);
			const double d = r / s;
			const double sinc = sin(PI * d) / (PI * d);
			const double sinc2 = sinc * sinc;

			if (d < 0.99)
			{
				out_RS(x,y) /= sinc2;
			}
			else
			{
				out_RS(x,y) = 0.0;
			}
		}
	}

	return out_RS;
}
