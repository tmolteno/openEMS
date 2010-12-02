/*
*	Copyright (C) 2010 Thorsten Liebig (Thorsten.Liebig@gmx.de)
*
*	This program is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*
*	This program is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "openems.h"
#include <iomanip>
#include "tools/array_ops.h"
#include "FDTD/operator_cylinder.h"
#include "FDTD/operator_cylindermultigrid.h"
#include "FDTD/engine_multithread.h"
#include "FDTD/operator_multithread.h"
#include "FDTD/operator_ext_mur_abc.h"
#include "FDTD/operator_ext_pml_sf.h"
#include "FDTD/operator_ext_upml.h"
#include "FDTD/engine_interface_fdtd.h"
#include "FDTD/processvoltage.h"
#include "FDTD/processcurrent.h"
#include "FDTD/process_efield.h"
#include "FDTD/process_hfield.h"
#include "FDTD/processmodematch.h"
#include "FDTD/processfields_td.h"
#include <sys/time.h>
#include <time.h>
#include <H5Cpp.h> // only for H5get_libversion()
#include <boost/version.hpp> // only for BOOST_LIB_VERSION

#include "FDTD/operator_ext_lorentzmaterial.h"

//external libs
#include "tinyxml.h"
#include "ContinuousStructure.h"

double CalcDiffTime(timeval t1, timeval t2)
{
	double s_diff = t1.tv_sec - t2.tv_sec;
	s_diff += (t1.tv_usec-t2.tv_usec)*1e-6;
	return s_diff;
}

openEMS::openEMS()
{
	FDTD_Op=NULL;
	FDTD_Eng=NULL;
	PA=NULL;
	CylinderCoords = false;
	Enable_Dumps = true;
	DebugMat = false;
	DebugOp = false;
	m_debugCSX = false;
	m_debugBox = m_debugPEC = m_no_simulation = false;
	endCrit = 1e-6;
	m_OverSampling = 4;

	m_engine = EngineType_Standard;
	m_engine_numThreads = 0;

	m_Abort = false;
}

openEMS::~openEMS()
{
	Reset();
}

void openEMS::Reset()
{
	if (PA) PA->DeleteAll();
	delete PA; PA=0;
	delete FDTD_Eng; FDTD_Eng=0;
	delete FDTD_Op; FDTD_Op=0;
}

//! \brief processes a command line argument
//! \return true if argument is known
//! \return false if argument is unknown
bool openEMS::parseCommandLineArgument( const char *argv )
{
	if (!argv)
		return false;

	if (strcmp(argv,"--disable-dumps")==0)
	{
		cout << "openEMS - disabling all field dumps" << endl;
		SetEnableDumps(false);
		return true;
	}
	else if (strcmp(argv,"--debug-material")==0)
	{
		cout << "openEMS - dumping material to 'material_dump.vtk'" << endl;
		DebugMaterial();
		return true;
	}
	else if (strcmp(argv,"--debug-operator")==0)
	{
		cout << "openEMS - dumping operator to 'operator_dump.vtk'" << endl;
		DebugOperator();
		return true;
	}
	else if (strcmp(argv,"--debug-boxes")==0)
	{
		cout << "openEMS - dumping boxes to 'box_dump*.vtk'" << endl;
		DebugBox();
		return true;
	}
	else if (strcmp(argv,"--debug-PEC")==0)
	{
		cout << "openEMS - dumping PEC info to 'PEC_dump.vtk'" << endl;
		m_debugPEC = true;
		return true;
	}
	else if (strcmp(argv,"--debug-CSX")==0)
	{
		cout << "openEMS - dumping CSX geometry to 'debugCSX.xml'" << endl;
		m_debugCSX = true;
		return true;
	}
	else if (strcmp(argv,"--engine=multithreaded")==0)
	{
		cout << "openEMS - enabled multithreading" << endl;
		m_engine = EngineType_Multithreaded;
		return true;
	}
	else if (strncmp(argv,"--numThreads=",13)==0)
	{
		m_engine_numThreads = atoi(argv+13);
		cout << "openEMS - fixed number of threads: " << m_engine_numThreads << endl;
		return true;
	}
	else if (strcmp(argv,"--engine=sse")==0)
	{
		cout << "openEMS - enabled sse engine" << endl;
		m_engine = EngineType_SSE;
		return true;
	}
	else if (strcmp(argv,"--engine=sse-compressed")==0)
	{
		cout << "openEMS - enabled compressed sse engine" << endl;
		m_engine = EngineType_SSE_Compressed;
		return true;
	}
	else if (strcmp(argv,"--engine=fastest")==0)
	{
		cout << "openEMS - enabled multithreading engine" << endl;
		m_engine = EngineType_Multithreaded;
		return true;
	}
	else if (strcmp(argv,"--no-simulation")==0)
	{
		cout << "openEMS - disabling simulation => preprocessing only" << endl;
		m_no_simulation = true;
		return true;
	}

	return false;
}

string openEMS::GetExtLibsInfo()
{
	stringstream str;

	str << "\tUsed external libraries:" << endl;
	str << "\t\t" << ContinuousStructure::GetInfoLine(true) << endl;

	// libhdf5
	unsigned int major, minor, release;
	if (H5get_libversion( &major, &minor, &release ) >= 0)
	{
		str << "\t\t" << "hdf5   -- Version: " << major << '.' << minor << '.' << release << endl;
		str << "\t\t" << "          compiled against: " H5_VERS_INFO << endl;
	}

	// tinyxml
	str << "\t\t" << "tinyxml -- compiled against: " << TIXML_MAJOR_VERSION << '.' << TIXML_MINOR_VERSION << '.' << TIXML_PATCH_VERSION << endl;

	// boost
	str << "\t\t" << "boost  -- compiled against: " BOOST_LIB_VERSION << endl;

	return str.str();
}

bool openEMS::SetupBoundaryConditions(TiXmlElement* BC)
{
	int EC; //error code of tinyxml
	int bounds[6] = {0,0,0,0,0,0};   //default boundary cond. (PEC)
	unsigned int pml_size[6] = {8,8,8,8,8,8}; //default pml size
	string s_bc;
	const char* tmp = BC->Attribute("PML_Grading");
	string pml_gradFunc;
	if (tmp)
		pml_gradFunc = string(tmp);

	string bound_names[] = {"xmin","xmax","ymin","ymax","zmin","zmax"};

	for (int n=0;n<6;++n)
	{
		EC = BC->QueryIntAttribute(bound_names[n].c_str(),&bounds[n]);
		if (EC==TIXML_SUCCESS)
			continue;
		if (EC==TIXML_WRONG_TYPE)
		{
			tmp = BC->Attribute(bound_names[n].c_str());
			if (tmp)
				s_bc = string(tmp);
			if (s_bc=="PEC")
				bounds[n] = 0;
			else if (s_bc=="PMC")
				bounds[n] = 1;
			else if (s_bc=="MUR")
				bounds[n] = 2;
			else if(strncmp(s_bc.c_str(),"PML_=",4)==0)
			{
				bounds[n] = 3;
				pml_size[n] = atoi(s_bc.c_str()+4);
			}
			else
				cerr << "openEMS::SetupBoundaryConditions: Warning,  boundary condition for \"" << bound_names[n] << "\" unknown... set to PEC " << endl;
		}
		else
			cerr << "openEMS::SetupBoundaryConditions: Warning, boundary condition for \"" << bound_names[n] << "\" not found... set to PEC " << endl;
	}

	FDTD_Op->SetBoundaryCondition(bounds); //operator only knows about PEC and PMC, everything else is defined by extensions (see below)

	/**************************** create all operator/engine extensions here !!!! **********************************/
	//Mur-ABC, defined as extension to the operator
	double mur_v_ph = 0;
	//read general mur phase velocity
	if (BC->QueryDoubleAttribute("MUR_PhaseVelocity",&mur_v_ph) != TIXML_SUCCESS)
		mur_v_ph = -1;
	string mur_v_ph_names[6] = {"MUR_PhaseVelocity_xmin", "MUR_PhaseVelocity_xmax", "MUR_PhaseVelocity_ymin", "MUR_PhaseVelocity_ymax", "MUR_PhaseVelocity_zmin", "MUR_PhaseVelocity_zmax"};
	for (int n=0;n<6;++n)
	{
		if (bounds[n]==2) //Mur-ABC
		{
			Operator_Ext_Mur_ABC* op_ext_mur = new Operator_Ext_Mur_ABC(FDTD_Op);
			op_ext_mur->SetDirection(n/2,n%2);
			double v_ph = 0;
			//read special mur phase velocity or assign general phase velocity
			if (BC->QueryDoubleAttribute(mur_v_ph_names[n].c_str(),&v_ph) == TIXML_SUCCESS)
				op_ext_mur->SetPhaseVelocity(v_ph);
			else if (mur_v_ph>0)
				op_ext_mur->SetPhaseVelocity(mur_v_ph);
			FDTD_Op->AddExtension(op_ext_mur);
		}
	}

	//create the upml
	Operator_Ext_UPML::Create_UPML(FDTD_Op,bounds,pml_size,pml_gradFunc);

	return true;
}

int openEMS::SetupFDTD(const char* file)
{
	if (file==NULL) return -1;
	Reset();

	cout << "Read openEMS xml file: " << file << " ..." << endl;

	timeval startTime;
	gettimeofday(&startTime,NULL);


	TiXmlDocument doc(file);
	if (!doc.LoadFile())
	{
		cerr << "openEMS: Error File-Loading failed!!! File: " << file << endl;
		exit(-1);
	}

	cout << "Read openEMS Settings..." << endl;
	TiXmlElement* openEMSxml = doc.FirstChildElement("openEMS");
	if (openEMSxml==NULL)
	{
		cerr << "Can't read openEMS ... " << endl;
		exit(-1);
	}

	TiXmlElement* FDTD_Opts = openEMSxml->FirstChildElement("FDTD");

	if (FDTD_Opts==NULL)
	{
		cerr << "Can't read openEMS FDTD Settings... " << endl;
		exit(-1);
	}
	int help=0;
	FDTD_Opts->QueryIntAttribute("NumberOfTimesteps",&help);
	if (help<0)
		NrTS=0;
	else
		NrTS = help;

	help = 0;
	FDTD_Opts->QueryIntAttribute("CylinderCoords",&help);
	if (help==1)
	{
//		cout << "Using a cylinder coordinate FDTD..." << endl;
		CylinderCoords = true;
	}

	FDTD_Opts->QueryDoubleAttribute("endCriteria",&endCrit);
	if (endCrit==0)
		endCrit=1e-6;

	FDTD_Opts->QueryIntAttribute("OverSampling",&m_OverSampling);
	if (m_OverSampling<2)
		m_OverSampling=2;

	double maxTime=0;
	FDTD_Opts->QueryDoubleAttribute("MaxTime",&maxTime);

	TiXmlElement* BC = FDTD_Opts->FirstChildElement("BoundaryCond");
	if (BC==NULL)
	{
		cerr << "Can't read openEMS boundary cond Settings... " << endl;
		exit(-3);
	}

	cout << "Read Geometry..." << endl;
	ContinuousStructure CSX;
	string EC(CSX.ReadFromXML(openEMSxml));
	if (EC.empty()==false)
	{
		cerr << EC << endl;
//		return(-2);
	}

	if (CylinderCoords)
		if (CSX.GetCoordInputType()!=CYLINDRICAL)
		{
		cerr << "openEMS::SetupFDTD: Warning: Coordinate system found in the CSX file is not a cylindrical. Forcing to cylindrical coordinate system!" << endl;
			CSX.SetCoordInputType(CYLINDRICAL); //tell CSX to use cylinder-coords
		}

	if (m_debugCSX)
		CSX.Write2XML("debugCSX.xml");

	//*************** setup operator ************//
	if (CylinderCoords)
	{
		const char* radii = FDTD_Opts->Attribute("MultiGrid");
		if (radii)
		{
			string rad(radii);
			FDTD_Op = Operator_CylinderMultiGrid::New(SplitString2Double(rad,','),m_engine_numThreads);
		}
		else
			FDTD_Op = Operator_Cylinder::New(m_engine_numThreads);
	}
	else if (m_engine == EngineType_SSE)
	{
		FDTD_Op = Operator_sse::New();
	}
	else if (m_engine == EngineType_SSE_Compressed)
	{
		FDTD_Op = Operator_SSE_Compressed::New();
	}
	else if (m_engine == EngineType_Multithreaded)
	{
		FDTD_Op = Operator_Multithread::New(m_engine_numThreads);
	}
	else
	{
		FDTD_Op = Operator::New();
	}

	if (FDTD_Op->SetGeometryCSX(&CSX)==false) return(2);

	SetupBoundaryConditions(BC);

	if (CSX.GetQtyPropertyType(CSProperties::LORENTZMATERIAL)>0)
		FDTD_Op->AddExtension(new Operator_Ext_LorentzMaterial(FDTD_Op));

	double timestep=0;
	FDTD_Opts->QueryDoubleAttribute("TimeStep",&timestep);
	if (timestep)
		FDTD_Op->SetTimestep(timestep);

	Operator::DebugFlags debugFlags = Operator::None;
	if (DebugMat)
		debugFlags |= Operator::debugMaterial;
	if (DebugOp)
		debugFlags |= Operator::debugOperator;
	if (m_debugPEC)
		debugFlags |= Operator::debugPEC;
	FDTD_Op->CalcECOperator( debugFlags );
	
	unsigned int maxTime_TS = (unsigned int)(maxTime/FDTD_Op->GetTimestep());
	if ((maxTime_TS>0) && (maxTime_TS<NrTS))
		NrTS = maxTime_TS;

	if (!FDTD_Op->SetupExcitation( FDTD_Opts->FirstChildElement("Excitation"), NrTS ))
		exit(2);

	timeval OpDoneTime;
	gettimeofday(&OpDoneTime,NULL);

	FDTD_Op->ShowStat();
	FDTD_Op->ShowExtStat();

	cout << "Creation time for operator: " << CalcDiffTime(OpDoneTime,startTime) << " s" << endl;

	if (m_no_simulation) {
		// simulation was disabled (to generate debug output only)
		return 1;
	}

	//create FDTD engine
	FDTD_Eng = FDTD_Op->CreateEngine();

	//*************** setup processing ************//
	cout << "Setting up processing..." << endl;

	unsigned int Nyquist = FDTD_Op->Exc->GetNyquistNum();
	PA = new ProcessingArray(Nyquist);

	double start[3];
	double stop[3];
	vector<CSProperties*> Probes = CSX.GetPropertyByType(CSProperties::PROBEBOX);
	for (size_t i=0;i<Probes.size();++i)
	{
		//only looking for one prim atm
		CSPrimitives* prim = Probes.at(i)->GetPrimitive(0);
		if (prim!=NULL)
		{
			bool acc;
			double bnd[6] = {0,0,0,0,0,0};
			acc = prim->GetBoundBox(bnd,true);
			start[0]= bnd[0];start[1]=bnd[2];start[2]=bnd[4];
			stop[0] = bnd[1];stop[1] =bnd[3];stop[2] =bnd[5];
			CSPropProbeBox* pb = Probes.at(i)->ToProbeBox();
			Processing* proc = NULL;
			if (pb)
			{
				if (pb->GetProbeType()==0)
				{
					ProcessVoltage* procVolt = new ProcessVoltage(FDTD_Op);
					proc=procVolt;
				}
				else if (pb->GetProbeType()==1)
				{
					ProcessCurrent* procCurr = new ProcessCurrent(FDTD_Op);
					proc=procCurr;
				}
				else if (pb->GetProbeType()==2)
					proc = new ProcessEField(FDTD_Op,FDTD_Eng);
				else if (pb->GetProbeType()==3)
					proc = new ProcessHField(FDTD_Op,FDTD_Eng);
				else if ((pb->GetProbeType()==10) || (pb->GetProbeType()==11))
				{
					ProcessModeMatch* pmm = new ProcessModeMatch(FDTD_Op);
					pmm->SetFieldType(pb->GetProbeType()-10);
					pmm->SetModeFunction(0,pb->GetAttributeValue("ModeFunctionX"));
					pmm->SetModeFunction(1,pb->GetAttributeValue("ModeFunctionY"));
					pmm->SetModeFunction(2,pb->GetAttributeValue("ModeFunctionZ"));
					proc = pmm;
				}
				else
				{
					cerr << "openEMS::SetupFDTD: Warning: Probe type " << pb->GetProbeType() << " of property '" << pb->GetName() << "' is unknown..." << endl;
					continue;
				}
				if (CylinderCoords)
					proc->SetMeshType(Processing::CYLINDRICAL_MESH);
				proc->SetEngineInterface(new Engine_Interface_FDTD(FDTD_Op,FDTD_Eng));
				proc->SetProcessInterval(Nyquist/m_OverSampling);
				proc->AddFrequency(pb->GetFDSamples());
				proc->SetName(pb->GetName());
				proc->DefineStartStopCoord(start,stop);
				proc->SetWeight(pb->GetWeighting());
				proc->InitProcess();
				PA->AddProcessing(proc);
				prim->SetPrimitiveUsed(true);
			}
			else
				delete 	proc;
		}
	}

	vector<CSProperties*> DumpProps = CSX.GetPropertyByType(CSProperties::DUMPBOX);
	for (size_t i=0;i<DumpProps.size();++i)
	{
		ProcessFieldsTD* ProcTD = new ProcessFieldsTD(FDTD_Op);
		ProcTD->SetEnable(Enable_Dumps);
		ProcTD->SetProcessInterval(Nyquist/m_OverSampling);

		//only looking for one prim atm
		CSPrimitives* prim = DumpProps.at(i)->GetPrimitive(0);
		if (prim==NULL)
			delete 	ProcTD;
		else
		{
			bool acc;
			double bnd[6] = {0,0,0,0,0,0};
			acc = prim->GetBoundBox(bnd,true);
			start[0]= bnd[0];start[1]=bnd[2];start[2]=bnd[4];
			stop[0] = bnd[1];stop[1] =bnd[3];stop[2] =bnd[5];
			CSPropDumpBox* db = DumpProps.at(i)->ToDumpBox();
			if (db)
			{
				ProcTD->SetEngineInterface(new Engine_Interface_FDTD(FDTD_Op,FDTD_Eng));
				ProcTD->SetDumpType((ProcessFields::DumpType)db->GetDumpType());
				ProcTD->SetDumpMode((Engine_Interface_Base::InterpolationType)db->GetDumpMode());
				ProcTD->SetFileType((ProcessFields::FileType)db->GetFileType());
				if (CylinderCoords)
					ProcTD->SetMeshType(Processing::CYLINDRICAL_MESH);
				for (int n=0;n<3;++n)
					ProcTD->SetSubSampling(db->GetSubSampling(n),n);
				ProcTD->SetFilePattern(db->GetName());
				ProcTD->SetFileName(db->GetName());
				ProcTD->DefineStartStopCoord(start,stop);
				ProcTD->InitProcess();
				PA->AddProcessing(ProcTD);
				prim->SetPrimitiveUsed(true);
			}
			else
				delete 	ProcTD;
		}
	}

	CSX.WarnUnusedPrimitves(cerr);

	// dump all boxes (voltage, current, fields, ...)
	if (m_debugBox)
	{
		PA->DumpBoxes2File("box_dump_");
	}

	return 0;
}

string FormatTime(int sec)
{
	stringstream ss;
	if (sec<60)
	{
		ss << setw(9) << sec << "s";
		return ss.str();
	}
	if (sec<3600)
	{
		ss << setw(6) << sec/60 << "m" << setw(2) << setfill('0') << sec%60 << "s";
		return ss.str();
	}
	ss << setw(3) << sec/3600 << "h" << setw(2) << setfill('0')  << (sec%3600)/60 << "m" << setw(2) << setfill('0')  << sec%60 << "s";
	return ss.str();
}

bool openEMS::CheckAbortCond()
{
	if (m_Abort) //abort was set externally
		return true;

	//check whether the file "ABORT" exist in current working directory
	ifstream ifile("ABORT");
	if (ifile)
	{
		ifile.close();
		cerr << "openEMS::CheckAbortCond(): Found file \"ABORT\", aborting simulation..." << endl;
		return true;
	}

	return false;
}

void openEMS::RunFDTD()
{
	cout << "Running FDTD engine... this may take a while... grab a cup of coffee?!?" << endl;

	//special handling of a field processing, needed to realize the end criteria...
	ProcessFields* ProcField = new ProcessFields(FDTD_Op);
	ProcField->SetEngineInterface(new Engine_Interface_FDTD(FDTD_Op,FDTD_Eng));
	PA->AddProcessing(ProcField);
	double maxE=0,currE=0;

	//add all timesteps to end-crit field processing with max excite amplitude
	unsigned int maxExcite = FDTD_Op->Exc->GetMaxExcitationTimestep();
	for (unsigned int n=0;n<FDTD_Op->Exc->Volt_Count;++n)
		ProcField->AddStep(FDTD_Op->Exc->Volt_delay[n]+maxExcite);

	double change=1;
	int prevTS=0,currTS=0;
	double speed = FDTD_Op->GetNumberCells()/1e6;
	double t_diff;

	timeval currTime;
	gettimeofday(&currTime,NULL);
	timeval startTime = currTime;
	timeval prevTime= currTime;

	//*************** simulate ************//

	int step=PA->Process();
	if ((step<0) || (step>(int)NrTS)) step=NrTS;
	while ((FDTD_Eng->GetNumberOfTimesteps()<NrTS) && (change>endCrit) && !CheckAbortCond())
	{
		FDTD_Eng->IterateTS(step);
		step=PA->Process();

		if (ProcField->CheckTimestep())
		{
			currE = ProcField->CalcTotalEnergy();
			if (currE>maxE)
				maxE=currE;
		}

//		cout << " do " << step << " steps; current: " << eng.GetNumberOfTimesteps() << endl;
		currTS = FDTD_Eng->GetNumberOfTimesteps();
		if ((step<0) || (step>(int)(NrTS - currTS))) step=NrTS - currTS;

		gettimeofday(&currTime,NULL);

		t_diff = CalcDiffTime(currTime,prevTime);
		if (t_diff>4)
		{
			currE = ProcField->CalcTotalEnergy();
			if (currE>maxE)
				maxE=currE;
			cout << "[@" << FormatTime(CalcDiffTime(currTime,startTime))  <<  "] Timestep: " << setw(12)  << currTS << " (" << setw(6) << setprecision(2) << std::fixed << (double)currTS/(double)NrTS*100.0  << "%)" ;
			cout << " || Speed: " << setw(6) << setprecision(1) << std::fixed << speed*(currTS-prevTS)/t_diff << " MC/s (" <<  setw(4) << setprecision(3) << std::scientific << t_diff/(currTS-prevTS) << " s/TS)" ;
			if (maxE)
				change = currE/maxE;
			cout << " || Energy: ~" << setw(6) << setprecision(2) << std::scientific << currE << " (-" << setw(5)  << setprecision(2) << std::fixed << fabs(10.0*log10(change)) << "dB)" << endl;
			prevTime=currTime;
			prevTS=currTS;

			PA->FlushNext();
		}
	}

	//*************** postproc ************//
	prevTime = currTime;
	gettimeofday(&currTime,NULL);

	t_diff = CalcDiffTime(currTime,startTime);

	cout << "Time for " << FDTD_Eng->GetNumberOfTimesteps() << " iterations with " << FDTD_Op->GetNumberCells() << " cells : " << t_diff << " sec" << endl;
	cout << "Speed: " << speed*(double)FDTD_Eng->GetNumberOfTimesteps()/t_diff << " MCells/s " << endl;
}
