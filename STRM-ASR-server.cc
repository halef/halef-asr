// CASSANDRA STREAMING ONLINE WS ASR SERVER
// Alexei V. Ivannov 2015
// Patrick Lange 2018

// Kaldi-related includes
#include "feat/wave-reader.h"
#include "online2/online-nnet2-decoding.h"
#include "online2/online-nnet2-feature-pipeline.h"
#include "online2/onlinebin-util.h"
#include "online2/online-timing.h"
#include "online2/online-endpoint.h"
#include "fstext/fstext-lib.h"
#include "lat/lattice-functions.h"
#include "util/kaldi-thread.h"

//Web socket-related includes
#include "libwebsockets.h"
//#pragma comment(lib, "websockets.lib")

//Multi-threading-related includes
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <fstream>

//Server configuration definitions
#define TIME_INTERVAL 50

const int DEFAULT_SERVER_PORT = 8008;
//const std::string DEFAULT_AUDIO_DIR = "processed_streams";
const std::string DEFAULT_AUDIO_DIR = "";

int32 server_port = DEFAULT_SERVER_PORT;
std::string audio_dir = DEFAULT_AUDIO_DIR;
std::string pidFileName = "";
int32 pid = -1;




namespace kaldi {

//communication layer  (a hack which can be only used in a single client mode)
float*			INPUT_RING_BUFFER;
int			INPUT_RING_BUFFER_SIZE;
int			INPUT_RING_BUFFER_RDPTR;
int			INPUT_RING_BUFFER_WRPTR;
  
//relevant connection (a hack which can be only used in a single client mode)
struct libwebsocket* myWSI;
  
// asr engine context
typedef struct
{
    std::string					word_syms_rxfilename;   
    OnlineEndpointConfig			endpoint_config;
    OnlineNnet2FeaturePipelineConfig		feature_config;  
    OnlineNnet2DecodingConfig			nnet2_decoding_config;

    BaseFloat					chunk_length_secs;
    bool					do_endpointing;
    bool					online;
    
    std::string					nnet2_rxfilename;
    std::string					fst_rxfilename;
    
    OnlineNnet2FeaturePipelineInfo*		feature_info;
    
    TransitionModel				trans_model;
    nnet2::AmNnet				nnet;

    fst::Fst<fst::StdArc>*			decode_fst;   
    fst::SymbolTable*				word_syms;
    
    int32					num_done;
    int32					num_err;
    double					tot_like;
    int64					num_frames;
    
    OnlineIvectorExtractorAdaptationState*	adaptation_state;
    OnlineNnet2FeaturePipeline*			feature_pipeline;
    OnlineSilenceWeighting*			silence_weighting;
    SingleUtteranceNnet2Decoder*		decoder;
    
    std::string spk;
    std::string utt; 

} ASRDATA;  
  

// EVENTS
int TIMER_EVENT=0;
pthread_cond_t timer_codition;
int EOC_EVENT=0;
pthread_cond_t asr_done_codition;

// MUTEXES
pthread_mutex_t timer_event_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t EOC_event_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t dummy_asr_done_mutex=PTHREAD_MUTEX_INITIALIZER;

//*********************
//*** FUNCTION TO MAKE INT TO STRING EASY ****
//*********************

namespace patch
{
  template < typename T > std::string to_string( const T& n )
  {
    std::ostringstream stm ;
    stm << n ;
    return stm.str() ;
  }
}


//*********************
//*** TIMER PROCESS ***
//*********************
void timer_process(struct timespec* SLptr)
{	
	//sleep(TIME_INTERVAL); // sleep takes int argumnet num of seconds!
	nanosleep(SLptr,NULL);
	pthread_mutex_lock(&timer_event_mutex);
	//if (TIMER_EVENT == 1) printf("### W A R N I N G ! ### xRT > 1\n");
	//if (TIMER_EVENT == 1) cntr++;	if(cntr>5) pthread_cond_broadcast(&asr_done_codition);
	TIMER_EVENT = 1;
	pthread_mutex_unlock(&timer_event_mutex);
	pthread_cond_broadcast(&timer_codition);
}

void *timer_function(void *arg)
{
	//int cntr=0;

	struct timespec SLEEPTIME;
	SLEEPTIME.tv_sec=0;
	SLEEPTIME.tv_nsec=TIME_INTERVAL*10000000; // TIME_INTERVAL*10 ms sleep-time

	while (1)
	{
		//pthread_mutex_lock(&EOC_event_mutex);
		//int EOC_EVENT_local=EOC_EVENT;
		//pthread_mutex_unlock(&EOC_event_mutex);

		//if(EOC_EVENT_local==0)	
		timer_process(&SLEEPTIME);
		//else
		//{
		//	// One last time - just to be sure
		//	timer_process(&SLEEPTIME);
		//	break;		
		//}
	}
  return 0;
}

//*******************
//*** ASR PROCESS ***
//*******************
void GetDiagnosticsAndPrintOutput(const std::string &utt,
                                  const fst::SymbolTable *word_syms,
                                  const CompactLattice &clat,
                                  int64 *tot_num_frames,
                                  double *tot_like,
				  struct libwebsocket* wsi
 				) {
  if (clat.NumStates() == 0) {
    std::cout << "Empty lattice.\n";
    return;
  }else{std::cout << "There are " << clat.NumStates() << " lattice states.\n";}
    
  CompactLattice best_path_clat;
  CompactLatticeShortestPath(clat, &best_path_clat);
  
  Lattice best_path_lat;
  ConvertLattice(best_path_clat, &best_path_lat);
  
  double likelihood;
  LatticeWeight weight;
  int32 num_frames;
  std::vector<int32> alignment;
  std::vector<int32> words;
  GetLinearSymbolSequence(best_path_lat, &alignment, &words, &weight);
  num_frames = alignment.size();
  likelihood = -(weight.Value1() + weight.Value2());
  *tot_num_frames += num_frames;
  *tot_like += likelihood;
  std::cout << "Likelihood per frame for utterance " << utt << " is " << (likelihood / num_frames) << " over " << num_frames << " frames.\n";
             
  if (word_syms != NULL) {
    std::cerr << utt << ' ';
    for (size_t i = 0; i < words.size(); i++) {
      std::string s = word_syms->Find(words[i]);
      if (s == "")
        std::cout << "Word-id " << words[i] << " not in symbol table.";
      std::cerr << s << ' ';
    }
    std::cerr << std::endl;
  }
  
  // HEAP ALLOCATIONS
  //char* TEXT_OUTPUT=(char*)malloc(20480); sprintf(TEXT_OUTPUT,"\0");
  std::string TEXT_OUTPUT="";
  // Write it into the socket also
  
  if (word_syms != NULL) {
    TEXT_OUTPUT="{ ";
    //printf("words.size()=%d\n",words.size());
    for (size_t i = 0; i < words.size(); i++) {
      std::string s = word_syms->Find(words[i]);
      if (s == "")
        KALDI_ERR << "Word-id " << words[i] << " not in symbol table.";
    TEXT_OUTPUT+= s; TEXT_OUTPUT+=" ";
    }
    TEXT_OUTPUT+="}";
  }


	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 65536];
	int n, m;
	unsigned char *p = &buf[LWS_SEND_BUFFER_PRE_PADDING];
  n = sprintf((char *)p, "%s", TEXT_OUTPUT.c_str());
  
  libwebsocket_write(wsi, p, n, LWS_WRITE_TEXT);

}


int asr_construct(ASRDATA* ASR)
{
   printf("***asr_construct\n");

   ASR->spk = "speaker1";
   ASR->utt = "utterance1";
   
   return 0;

}

int asr_init(ASRDATA* ASR)
{
  printf("***asr_init\n");
  //printf("ASR->feature_info->ivector_extractor_info=\'%s\'\n",ASR->feature_info->ivector_extractor_info);
  ASR->adaptation_state=new OnlineIvectorExtractorAdaptationState(ASR->feature_info->ivector_extractor_info);
  ASR->feature_pipeline=new OnlineNnet2FeaturePipeline(*(ASR->feature_info));
  ASR->feature_pipeline->SetAdaptationState(*(ASR->adaptation_state));
  ASR->silence_weighting=new OnlineSilenceWeighting(ASR->trans_model,ASR->feature_info->silence_weighting_config);
  ASR->decoder=new SingleUtteranceNnet2Decoder(ASR->nnet2_decoding_config, ASR->trans_model, ASR->nnet, *(ASR->decode_fst), ASR->feature_pipeline);
  
  ASR->num_done = 0;
  ASR->num_err = 0;
  ASR->tot_like = 0.0;
  ASR->num_frames = 0;
  
  return 0;
  
}
  
int asr_process(ASRDATA* ASR,int EOCstate)
{
  
    int finished=0;
    
    BaseFloat samp_freq = 8000.0;

    int32 chunk_length;
    if (ASR->chunk_length_secs > 0) {
      chunk_length = int32(samp_freq * ASR->chunk_length_secs);
      if (chunk_length == 0) chunk_length = 1;
    } else {
      chunk_length = std::numeric_limits<int32>::max();
    }

    
    int local_maxptr=INPUT_RING_BUFFER_WRPTR;
    //INPUT_RING_BUFFER_RDPTR;
    BaseFloat* local_buffer=(BaseFloat*)malloc((local_maxptr-INPUT_RING_BUFFER_RDPTR)*sizeof(BaseFloat));
    //memcpy(INPUT_RING_BUFFER+INPUT_RING_BUFFER_RDPTR%INPUT_RING_BUFFER_SIZE,local_buffer,local_maxptr-INPUT_RING_BUFFER_RDPTR);
    //because it is a ringbuffer, we read samples one by one
    
    //printf("local_buffer size is %d floats\n",(local_maxptr-INPUT_RING_BUFFER_RDPTR));
    
    int num_chunks=floor((local_maxptr-INPUT_RING_BUFFER_RDPTR)/chunk_length);
    //printf("local_buffer= ");
    for(int i=0;i<num_chunks;i++) for(int k=0;k<chunk_length;k++){
      local_buffer[i*chunk_length+k]=*(INPUT_RING_BUFFER+(INPUT_RING_BUFFER_RDPTR+i*chunk_length+k)%INPUT_RING_BUFFER_SIZE);
      //printf("%f ",local_buffer[i*chunk_length+k]);
    }//printf("\n");
    SubVector<BaseFloat> data(local_buffer,num_chunks*chunk_length);
    
    
    //OnlineTimer decoding_timer(ASR->utt);
   
    int32 samp_offset = 0;
    std::vector<std::pair<int32, BaseFloat> > delta_weights;
    
    
    while (samp_offset < data.Dim()) {
      int32 samp_remaining = data.Dim() - samp_offset;
      int32 num_samp = chunk_length < samp_remaining ? chunk_length : samp_remaining;
      
      SubVector<BaseFloat> wave_part(data, samp_offset, num_samp);
      ASR->feature_pipeline->AcceptWaveform(samp_freq, wave_part);

      samp_offset += num_samp;

      if (ASR->silence_weighting->Active()) {
	      ASR->silence_weighting->ComputeCurrentTraceback(ASR->decoder->Decoder());
	      ASR->silence_weighting->GetDeltaWeights(ASR->feature_pipeline->NumFramesReady(),
					  &delta_weights);
	      ASR->feature_pipeline->IvectorFeature()->UpdateFrameWeights(delta_weights);
      }
      
      ASR->decoder->AdvanceDecoding();     
      //printf("samp_offset %d samples, data.Dim() %d\n",samp_offset,data.Dim());

    }
    

    //printf("asr_process has processed %d samples, chunk length is %d samples, loaded %d chunks\n",local_maxptr-INPUT_RING_BUFFER_RDPTR,chunk_length,num_chunks);

    INPUT_RING_BUFFER_RDPTR+=num_chunks*chunk_length;
    
    
    if(EOCstate && (INPUT_RING_BUFFER_WRPTR-INPUT_RING_BUFFER_RDPTR)<chunk_length){
      // We've reached the end of the utterance, No more NEW data to process, flush old buffers, update and generate the final best

      ASR->feature_pipeline->InputFinished();

      if (ASR->silence_weighting->Active()) {
	ASR->silence_weighting->ComputeCurrentTraceback(ASR->decoder->Decoder());
	ASR->silence_weighting->GetDeltaWeights(ASR->feature_pipeline->NumFramesReady(),
					  &delta_weights);
	ASR->feature_pipeline->IvectorFeature()->UpdateFrameWeights(delta_weights);
      }
      
      ASR->decoder->AdvanceDecoding();     


      ASR->decoder->FinalizeDecoding();

      CompactLattice clat;
      bool end_of_utterance = true;
      ASR->decoder->GetLattice(end_of_utterance, &clat);
      
      
      //printf("ASR->wordsyms=0x%x\n",ASR->word_syms);
      
      GetDiagnosticsAndPrintOutput(ASR->utt, ASR->word_syms, clat, &(ASR->num_frames), &(ASR->tot_like), myWSI);
      
      //decoding_timer.OutputStats(&timing_stats);
      
      // In an application you might avoid updating the adaptation state if
      // you felt the utterance had low confidence.  See lat/confidence.h
      //feature_pipeline.GetAdaptationState(&adaptation_state);    //AI: no adaptation state update (for the moment)
      
      // we want to output the lattice with un-scaled acoustics.
      //BaseFloat inv_acoustic_scale =					//AI: no lattice (for the moment)
      //    1.0 / nnet2_decoding_config.decodable_opts.acoustic_scale;
      //ScaleLattice(AcousticLatticeScale(inv_acoustic_scale), &clat);
      //clat_writer.Write(utt, clat);

      std::cout << "Decoded "<< ASR->num_frames<< " frames of the utterance " << ASR->utt << " with the total likelihood "<< ASR->tot_like <<"\n";
      finished=1;
    }

    free(local_buffer);

    if (finished) return 1;
    else return 0;

}

int asr_stop(ASRDATA* ASR)
{
   printf("***asr_stop\n");

   return 0;
}

int asr_reset(ASRDATA* ASR)
{
   printf("***asr_reset\n");
  
  delete ASR->adaptation_state;
  delete ASR->feature_pipeline;
  delete ASR->silence_weighting;
  delete ASR->decoder;
  
  ASR->adaptation_state=new OnlineIvectorExtractorAdaptationState(ASR->feature_info->ivector_extractor_info);
  ASR->feature_pipeline=new OnlineNnet2FeaturePipeline(*(ASR->feature_info));
  ASR->feature_pipeline->SetAdaptationState(*(ASR->adaptation_state));
  ASR->silence_weighting=new OnlineSilenceWeighting(ASR->trans_model,ASR->feature_info->silence_weighting_config);
  ASR->decoder=new SingleUtteranceNnet2Decoder(ASR->nnet2_decoding_config, ASR->trans_model, ASR->nnet, *(ASR->decode_fst), ASR->feature_pipeline);  
  
  ASR->tot_like = 0.0;
  ASR->num_frames = 0;

  pthread_mutex_lock(&EOC_event_mutex);
  EOC_EVENT=0;
  pthread_mutex_unlock(&EOC_event_mutex);

  return 0;
}


int asr_destroy(ASRDATA* ASR)
{
   printf("***asr_destroy\n");
//  delete ASR->adaptation_state;
  return 0;
}
  
void *asr_function(void* arg)
{

	ASRDATA* RECOGNIZER=(ASRDATA*) arg;

	int i=1;	
	//printf("$");

	asr_construct(RECOGNIZER);

	asr_init(RECOGNIZER);



	while(i==1)
	{

		pthread_mutex_lock(&EOC_event_mutex);
		int EOC_EVENT_local=EOC_EVENT;
		pthread_mutex_unlock(&EOC_event_mutex);

		//printf("#EOC_EVENT_local#=%d\n",EOC_EVENT_local);

		if(EOC_EVENT_local==0)
		{

			//User is stil calling
			pthread_mutex_lock(&timer_event_mutex);
			if (TIMER_EVENT == 0)
			{
				//Timer did not update event yet (xRT<1) ==> go to sleep
				pthread_mutex_unlock(&timer_event_mutex);
				pthread_cond_wait(&timer_codition,&timer_event_mutex);
				if(TIMER_EVENT == 0) {printf("ERR000. Error in Timer logic!\n");} 
				else {TIMER_EVENT = 0;}
			}
			else
			{
				//Timer already updated event yet (xRT>1) ==> go to work immediately
				TIMER_EVENT = 0;
			}

			pthread_mutex_unlock(&timer_event_mutex);

			asr_process(RECOGNIZER,0);	
		}
		else
		{
			//User has done with the call
			while(!asr_process(RECOGNIZER,1)){};
			asr_reset(RECOGNIZER);

			//i=0;
			
		}
	}

	asr_stop(RECOGNIZER);
	asr_destroy(RECOGNIZER);

	return 0;	
}

//*****************************
//*** AUDIO STREAMING LAYER ***
//*****************************

struct audio_streaming_protocol_instance {
	char marshalling[16384];
	int marshalled;
	int TOTAL_AUDIO_SIZE;
	FILE* fn;
};

static int 
callback_http(struct libwebsocket_context * that,
	struct libwebsocket *wsi,
	enum libwebsocket_callback_reasons reason, void *user,
	void *in, size_t len)
{
	return 0;
}

static int 
callback_audio_streaming(struct libwebsocket_context * that,
	struct libwebsocket *wsi,
	enum libwebsocket_callback_reasons reason,
	void *user, void *in, size_t len)
{
  
	struct audio_streaming_protocol_instance *ldi = (audio_streaming_protocol_instance *)user;
	
	time_t tnow;struct tm* now;char fnbuffer[80];
	
	switch (reason) {
	case LWS_CALLBACK_ESTABLISHED: // just log message that someone is connecting
		printf("connection established\n");
		ldi->marshalled = 0;
				
		break;
	case LWS_CALLBACK_CLOSED:
		fprintf(stderr, "LWS_CALLBACK_CLOSED\n");
		//delete ldi->adaptation_state;
		break;
	case LWS_CALLBACK_RECEIVE: {
	  
	  
		if(!ldi->fn){
		  tnow=time(0);   // get time now
		  now=localtime(&tnow);

		  std::string tmp = audio_dir  + "/" + patch::to_string(server_port) + "_stream_%F-%T.raw";
		  strftime (fnbuffer,80, tmp.c_str() ,now);
		  
		  ldi->fn=fopen(fnbuffer,"wb");
		  ldi->TOTAL_AUDIO_SIZE=0;
		  myWSI=wsi;
		  INPUT_RING_BUFFER_RDPTR=0;				//reading pointer
		  INPUT_RING_BUFFER_WRPTR=0;				//writing pointer
		}
		

		//FILE* f=fopen("test.data","wb+");
		// the funny part
		// create a buffer to hold our response
		// it has to have some pre and post padding. You don't need to care
		// what comes there, libwebsockets will do everything for you. For more info see
		// http://git.warmcat.com/cgi-bin/cgit/libwebsockets/tree/lib/libwebsockets.h#n597
		//unsigned char *buf = (unsigned char*) malloc(LWS_SEND_BUFFER_PRE_PADDING + len +
		//	LWS_SEND_BUFFER_POST_PADDING);

		// pointer to `void *in` holds the incomming request
		// we're just going to put it in reverse order and put it in `buf` with
		// correct offset. `len` holds length of the request.
		//for (size_t i=0; i < len; i++) {
		//	buf[LWS_SEND_BUFFER_PRE_PADDING + i ] = ((char *) in)[i];
		//}

		// log what we recieved and what we're going to send as a response.
		// that disco syntax `%.*s` is used to print just a part of our buffer
		// http://stackoverflow.com/questions/5189071/print-part-of-char-array
		//AI// printf("received data: %s, replying: %.*s\n", (char *) in, (int) len,	buf + LWS_SEND_BUFFER_PRE_PADDING);
		for(int i=0;i<len;i+=2){
		  //short tmp=0;
		  //memcpy((char*) in+i,&tmp,2);
		  INPUT_RING_BUFFER[INPUT_RING_BUFFER_WRPTR%INPUT_RING_BUFFER_SIZE]=(float)(*((short*)in+i/2));
		  INPUT_RING_BUFFER_WRPTR++;
		}
		fwrite(((char*) in),1,len,ldi->fn);
		// send response
		// just notice that we have to tell where exactly our response starts. That's
		// why there's `buf[LWS_SEND_BUFFER_PRE_PADDING]` and how long it is.
		// we know that our response has the same length as request because
		// it's the same message in reverse order.
		//AI// 
		ldi->TOTAL_AUDIO_SIZE+=len;
		char* REPLY=(char*)malloc(LWS_SEND_BUFFER_PRE_PADDING+1024+LWS_SEND_BUFFER_POST_PADDING);
		if (libwebsocket_is_final_fragment(wsi) && !libwebsockets_remaining_packet_payload(wsi)){ //EOC event
		  //sprintf(REPLY+LWS_SEND_BUFFER_PRE_PADDING,"Accepted chunk with %d bytes, total audio size is %d bytes, FINAL_AUDIO_SIZE=%d bytes.\0",len,ldi->TOTAL_AUDIO_SIZE,ldi->TOTAL_AUDIO_SIZE);		    
		  fflush (ldi->fn);
		  ldi->TOTAL_AUDIO_SIZE=0; //<= this is bad actually
		  //fclose(ldi->fn); <= this is bad actually
		  
		  fclose(ldi->fn);ldi->fn=0;
		  
		  pthread_mutex_lock(&EOC_event_mutex);
		  EOC_EVENT=1;
		  pthread_mutex_unlock(&EOC_event_mutex);

		  
		}else{
		  //sprintf(REPLY+LWS_SEND_BUFFER_PRE_PADDING,"Accepted chunk with %d bytes, total audio size is %d bytes.\0",len,ldi->TOTAL_AUDIO_SIZE);
		}
		//libwebsocket_write(wsi, REPLY+LWS_SEND_BUFFER_PRE_PADDING, strlen(REPLY+LWS_SEND_BUFFER_PRE_PADDING), LWS_WRITE_TEXT);
		
		// release memory back into the wild
		free(REPLY);
		//free(buf);
		//fclose(f);
		break;
	}
	case LWS_CALLBACK_HTTP_FILE_COMPLETION:{
	  fprintf(stderr, "LWS_CALLBACK_HTTP_FILE_COMPLETION\n");
	  fclose(ldi->fn);
	}
	default:
		break;
	}

	return 0;
}

static struct libwebsocket_protocols protocols[] = {
	/* first protocol must always be HTTP handler */
	{
		"http-only",   // name
		callback_http, // callback
		0              // per_session_data_size
	},
	{
		"audio-streaming-protocol", // protocol name - very important!
		callback_audio_streaming,   // callback
		sizeof(struct audio_streaming_protocol_instance), //session data size
		16384,                          // packet size // 4096(4k) DOES NOT WORK, but 8192(8K) SOMETIMES WORKS, 16384(16K), 32768(32K), 65536(64K), 131072(128K) work
	},
	{
		NULL, NULL, 0   /* End of list */
	}
};

}

//******************
//*** EXECUTABLE ***
//******************

int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    using namespace fst;
    
    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;
    
    ASRDATA* REC_CONFIG=new ASRDATA;
   
    const char *usage =
        "Reads in Web Socket stream and does online decoding with neural nets\n"
        "Note: some configuration values and inputs are\n"
        "set via config files whose filenames are passed as options\n"
        "\n"
        "Usage: STRM-ASR-server [options] <nnet2-in> <fst-in> "
        "<spk2utt-rspecifier> <wav-rspecifier> <lattice-wspecifier>\n";
    
    ParseOptions po(usage);
    
    REC_CONFIG->chunk_length_secs = 0.05;
    REC_CONFIG->do_endpointing = false;
    REC_CONFIG->online = true;
   
        
    po.Register("pid-filename", &pidFileName,
                "Filename into which to put pid. No file will be created if not set.");
    
    po.Register("port", &server_port,
                "Port number for server. Defaults to: " + patch::to_string(DEFAULT_SERVER_PORT));

    po.Register("audio-dir", &audio_dir,
                "Directory where temporary audio files are stored that are only meant for debugging. Defaults to: " + DEFAULT_AUDIO_DIR);
    

    po.Register("chunk-length", &(REC_CONFIG->chunk_length_secs),
                "Length of chunk size in seconds, that we process.  Set to <= 0 "
                "to use all input in one chunk.");
    po.Register("word-symbol-table", &(REC_CONFIG->word_syms_rxfilename),
                "Symbol table for words [for debug output]");
    po.Register("do-endpointing", &(REC_CONFIG->do_endpointing),
                "If true, apply endpoint detection");
    po.Register("online", &(REC_CONFIG->online),
                "You can set this to false to disable online iVector estimation "
                "and have all the data for each utterance used, even at "
                "utterance start.  This is useful where you just want the best "
                "results and don't care about online operation.  Setting this to "
                "false has the same effect as setting "
                "--use-most-recent-ivector=true and --greedy-ivector-extractor=true "
                "in the file given to --ivector-extraction-config, and "
                "--chunk-length=-1.");
     
    REC_CONFIG->feature_config.Register(&po);
    REC_CONFIG->nnet2_decoding_config.Register(&po);
    REC_CONFIG->endpoint_config.Register(&po);
    
    po.Read(argc, argv);

    
    if (po.NumArgs() != 2) {
      po.PrintUsage();
      return 1;
    }

    if(audio_dir.compare("") == 0){
      std::cerr << "ERROR: audio-dir was not set" << std::endl;
      po.PrintUsage();
      return 1;

    }
    
    if(pidFileName != ""){
      std::ofstream outfile;

      outfile.open(pidFileName.c_str(), std::ofstream::out);
      outfile << getpid(); 
      outfile.close();
    }

    REC_CONFIG->nnet2_rxfilename = po.GetArg(1);
    REC_CONFIG->fst_rxfilename = po.GetArg(2);
    
//        spk2utt_rspecifier = po.GetArg(3),
//        wav_rspecifier = po.GetArg(4),
//        clat_wspecifier = po.GetArg(5);
    
    REC_CONFIG->feature_info=new OnlineNnet2FeaturePipelineInfo(REC_CONFIG->feature_config);

    if (!REC_CONFIG->online) {
      REC_CONFIG->feature_info->ivector_extractor_info.use_most_recent_ivector = true;
      REC_CONFIG->feature_info->ivector_extractor_info.greedy_ivector_extractor = true;
      REC_CONFIG->chunk_length_secs = -1.0;
    }
    

    {
      bool binary;
      Input ki(REC_CONFIG->nnet2_rxfilename, &binary);
      REC_CONFIG->trans_model.Read(ki.Stream(), binary);
      REC_CONFIG->nnet.Read(ki.Stream(), binary);
    }
    
    REC_CONFIG->decode_fst = ReadFstKaldi(REC_CONFIG->fst_rxfilename);
    
    REC_CONFIG->word_syms = NULL;
    if (REC_CONFIG->word_syms_rxfilename != "")
      if (!(REC_CONFIG->word_syms = fst::SymbolTable::ReadText(REC_CONFIG->word_syms_rxfilename)))
        KALDI_ERR << "Could not read symbol table from file "
                  << REC_CONFIG->word_syms_rxfilename;
   
    // KALDI INITIALIZATION FINISHED
    
    
    // COMMUNICATION LAYER INITIALIZATION
    
    INPUT_RING_BUFFER_SIZE=50*8000;			//50 sec on 8KHz
    INPUT_RING_BUFFER_RDPTR=0;				//reading pointer
    INPUT_RING_BUFFER_WRPTR=0;				//writing pointer
    INPUT_RING_BUFFER=(float *)malloc(INPUT_RING_BUFFER_SIZE*sizeof(float));

    
    // THREADING INITIALIZATION
    
    pthread_t asr_thread;
    pthread_t timer_thread;
	    

    if(pthread_create(&timer_thread,NULL, timer_function, NULL)) {printf("ERR001. Error Creating Timer!\n");}
    int ret1 = pthread_detach(timer_thread);

    if(pthread_create(&asr_thread,NULL, asr_function, REC_CONFIG)) {printf("ERR002. Error Creating ASR!\n");}
    int ret2 = pthread_detach(asr_thread);


    struct libwebsocket_context *context;
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = server_port;
    info.gid = -1;
    info.uid = -1;
    info.protocols = protocols;
    //FILE** fn=malloc(4);

    // create libwebsocket context representing this server
    context = libwebsocket_create_context(&info);

    if (context == NULL) {
	    fprintf(stderr, "libwebsocket init failed\n");
	    return -1;
    }

    printf("starting server...\n");

    // infinite loop, to end this server send SIGTERM. (CTRL+C)
    while (1) {
	    libwebsocket_service(context, 50);
	    // libwebsocket_service will process all waiting events with their
	    // callback functions and then wait 50 ms.
	    // (this is a single threaded webserver and this will keep our server
	    // from generating load while there are not requests to process)
    }

    libwebsocket_context_destroy(context);
    //free(fn);


    delete REC_CONFIG->feature_info;
    delete REC_CONFIG->decode_fst;
    delete REC_CONFIG->word_syms; // will delete if non-NULL.
    return (REC_CONFIG->num_done != 0 ? 0 : 1);

    free(INPUT_RING_BUFFER);
    delete REC_CONFIG;
    
    pthread_cond_wait(&asr_done_codition,&dummy_asr_done_mutex);
    pthread_mutex_unlock(&dummy_asr_done_mutex);

    
  } catch(const std::exception& e) {
    std::cerr << e.what();
    return -1;
  }
} // main()
