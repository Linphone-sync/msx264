/*
mediastreamer2 x264 plugin
Copyright (C) 2006-2010 Belledonne Communications SARL (simon.morlat@linphone.org)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msticker.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/rfc3984.h"

#ifdef _MSC_VER
#include <stdint.h>
#endif

#include <x264.h>

#ifndef VERSION
#define VERSION "1.4.1"
#endif


#define RC_MARGIN 10000 /*bits per sec*/


#define SPECIAL_HIGHRES_BUILD_CRF 28


#define MS_X264_CONF(required_bitrate, bitrate_limit, resolution, fps, ncpus) \
	{ required_bitrate, bitrate_limit, { MS_VIDEO_SIZE_ ## resolution ## _W, MS_VIDEO_SIZE_ ## resolution ## _H }, fps, ncpus, NULL }


static const MSVideoConfiguration x264_conf_list[] = {
#if defined(ANDROID) || (TARGET_OS_IPHONE == 1) || defined(__arm__)
	MS_X264_CONF(2048000, 3072000,       UXGA, 15, 4),
	MS_X264_CONF(1024000, 1536000, SXGA_MINUS, 15, 4),
	MS_X264_CONF( 750000, 1024000,        XGA, 15, 4),
	MS_X264_CONF( 500000,  750000,       SVGA, 15, 2),
	MS_X264_CONF( 300000,  500000,        VGA, 12, 1),
	MS_X264_CONF( 170000,  300000,       QVGA, 12, 1),
	MS_X264_CONF( 128000,   170000,      QCIF, 10, 1),
	MS_X264_CONF(  64000,   128000,      QCIF,  7, 1),
	MS_X264_CONF(      0,    64000,      QCIF,  5 ,1)
#else
	MS_X264_CONF(1536000,  2560000, SXGA_MINUS, 18, 4),
	MS_X264_CONF(1536000,  2560000,       720P, 18, 4),
	MS_X264_CONF(1024000,  1536000,        XGA, 18, 4),
	MS_X264_CONF( 512000,  1024000,       SVGA, 18, 2),
	MS_X264_CONF( 256000,   512000,        VGA, 15, 1),
	MS_X264_CONF( 170000,   256000,       QVGA, 15, 1),
	MS_X264_CONF( 128000,   170000,       QCIF, 10, 1),
	MS_X264_CONF(  64000,   128000,       QCIF,  7, 1),
	MS_X264_CONF(      0,    64000,       QCIF,  5, 1)
#endif
};


/* the goal of this small object is to tell when to send I frames at startup:
at 2 and 4 seconds*/
typedef struct VideoStarter{
	uint64_t next_time;
	int i_frame_count;
}VideoStarter;

static void video_starter_init(VideoStarter *vs){
	vs->next_time=0;
	vs->i_frame_count=0;
}

static void video_starter_first_frame(VideoStarter *vs, uint64_t curtime){
	vs->next_time=curtime+2000;
}

static bool_t video_starter_need_i_frame(VideoStarter *vs, uint64_t curtime){
	if (vs->next_time==0) return FALSE;
	if (curtime>=vs->next_time){
		vs->i_frame_count++;
		if (vs->i_frame_count==1){
			vs->next_time+=2000;
		}else{
			vs->next_time=0;
		}
		return TRUE;
	}
	return FALSE;
}

typedef struct _EncData{
	x264_t *enc;
	x264_param_t params;
	int mode;
	uint64_t framenum;
	Rfc3984Context *packer;
	int keyframe_int;
	VideoStarter starter;
	const MSVideoConfiguration *vconf_list;
	MSVideoConfiguration vconf;
	bool_t generate_keyframe;
}EncData;


static void enc_init(MSFilter *f){
	EncData *d=ms_new(EncData,1);
	d->enc=NULL;
	d->keyframe_int=10; /*10 seconds */
	d->mode=0;
	d->framenum=0;
	d->generate_keyframe=FALSE;
	d->packer=NULL;
	d->vconf_list=x264_conf_list;
	d->vconf = ms_video_find_best_configuration_for_bitrate(d->vconf_list, 384000, ms_get_cpu_count());
	f->data=d;
}

static void enc_uninit(MSFilter *f){
	EncData *d=(EncData*)f->data;
	ms_free(d);
}

static void apply_bitrate(MSFilter *f){
	EncData *d=(EncData*)f->data;
	x264_param_t *params=&d->params;
	float bitrate;

	bitrate=(float)d->vconf.required_bitrate*0.92;
	if (bitrate>RC_MARGIN)
		bitrate-=RC_MARGIN;
	
	params->rc.i_rc_method = X264_RC_ABR;
	params->rc.i_bitrate=(int)(bitrate/1000);
	params->rc.f_rate_tolerance=0.1;
	params->rc.i_vbv_max_bitrate=(int) ((bitrate+RC_MARGIN/2)/1000);
	params->rc.i_vbv_buffer_size=params->rc.i_vbv_max_bitrate;
	params->rc.f_vbv_buffer_init=0.5;
}

static void enc_preprocess(MSFilter *f){
	EncData *d=(EncData*)f->data;
	x264_param_t *params=&d->params;
	
	d->packer=rfc3984_new();
	rfc3984_set_mode(d->packer,d->mode);
	rfc3984_enable_stap_a(d->packer,FALSE);
#if defined(__arm__) || defined(ANDROID)
	if (x264_param_default_preset(params,"superfast"/*"ultrafast"*/,"zerolatency")) {
		ms_error("Cannot apply default x264 configuration");
	}
#else
	x264_param_default(params);
#endif
	
	params->i_threads=ms_get_cpu_count();
	params->i_sync_lookahead=0;
	params->i_width=d->vconf.vsize.width;
	params->i_height=d->vconf.vsize.height;
	params->i_fps_num=(int)d->vconf.fps;
	params->i_fps_den=1;
	params->i_slice_max_size=ms_get_payload_max_size()-100; /*-100 security margin*/
	params->i_level_idc=13;
	
	apply_bitrate(f);

	params->rc.i_lookahead=0;
	/*enable this by config ?*/
	/*
	 params.i_keyint_max = (int)d->fps*d->keyframe_int;
	 params.i_keyint_min = (int)d->fps;
	 */
	params->b_repeat_headers=1;
	params->b_annexb=0;
	
	//these parameters must be set so that our stream is baseline
	params->analyse.b_transform_8x8 = 0;
	params->b_cabac = 0;
	params->i_cqm_preset = X264_CQM_FLAT;
	params->i_bframe = 0;
	params->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
	d->enc=x264_encoder_open(params);
	if (d->enc==NULL) ms_error("Fail to create x264 encoder.");
	d->framenum=0;
	video_starter_init(&d->starter);
}

static void x264_nals_to_msgb(x264_nal_t *xnals, int num_nals, MSQueue * nalus){
	int i;
	mblk_t *m;
	/*int bytes;*/
	for (i=0;i<num_nals;++i){
		m=allocb(xnals[i].i_payload+10,0);
		
		memcpy(m->b_wptr,xnals[i].p_payload+4,xnals[i].i_payload-4);
		m->b_wptr+=xnals[i].i_payload-4;
		if (xnals[i].i_type==7) {
			ms_message("A SPS is being sent.");
		}else if (xnals[i].i_type==8) {
			ms_message("A PPS is being sent.");
		}
		ms_queue_put(nalus,m);
	}
}

static void enc_process(MSFilter *f){
	EncData *d=(EncData*)f->data;
	uint32_t ts=f->ticker->time*90LL;
	mblk_t *im;
	MSPicture pic;
	MSQueue nalus;
	
	if (d->enc==NULL){
		ms_queue_flush(f->inputs[0]);
		return;
	}
	
	ms_queue_init(&nalus);
	while((im=ms_queue_get(f->inputs[0]))!=NULL){
		if (ms_yuv_buf_init_from_mblk(&pic,im)==0){
			x264_picture_t xpic;
			x264_picture_t oxpic;
			x264_nal_t *xnals=NULL;
			int num_nals=0;

			memset(&xpic, 0, sizeof(xpic));
			memset(&oxpic, 0, sizeof(oxpic));

			/*send I frame 2 seconds and 4 seconds after the beginning */
			if (video_starter_need_i_frame(&d->starter,f->ticker->time))
				d->generate_keyframe=TRUE;

			if (d->generate_keyframe){
				xpic.i_type=X264_TYPE_IDR;
				d->generate_keyframe=FALSE;
			}else xpic.i_type=X264_TYPE_AUTO;
			xpic.i_qpplus1=0;
			xpic.i_pts=d->framenum;
			xpic.param=NULL;
			xpic.img.i_csp=X264_CSP_I420;
			xpic.img.i_plane=3;
			xpic.img.i_stride[0]=pic.strides[0];
			xpic.img.i_stride[1]=pic.strides[1];
			xpic.img.i_stride[2]=pic.strides[2];
			xpic.img.i_stride[3]=0;
			xpic.img.plane[0]=pic.planes[0];
			xpic.img.plane[1]=pic.planes[1];
			xpic.img.plane[2]=pic.planes[2];
			xpic.img.plane[3]=0;

			if (x264_encoder_encode(d->enc,&xnals,&num_nals,&xpic,&oxpic)>=0){
				x264_nals_to_msgb(xnals,num_nals,&nalus);
				/*if (num_nals == 0)	ms_message("Delayed frames info: current=%d max=%d\n", 
					x264_encoder_delayed_frames(d->enc),
					x264_encoder_maximum_delayed_frames(d->enc));
				*/
				rfc3984_pack(d->packer,&nalus,f->outputs[0],ts);
				if (d->framenum==0)
					video_starter_first_frame(&d->starter,f->ticker->time);
				d->framenum++;
			}else{
				ms_error("x264_encoder_encode() error.");
			}
		}
		freemsg(im);
	}
}

static void enc_postprocess(MSFilter *f){
	EncData *d=(EncData*)f->data;
	rfc3984_destroy(d->packer);
	d->packer=NULL;
	if (d->enc!=NULL){
		x264_encoder_close(d->enc);
		d->enc=NULL;
	}
}

static int enc_get_br(MSFilter *f, void*arg){
	EncData *d=(EncData*)f->data;
	*(int*)arg=d->vconf.required_bitrate;
	return 0;
}

static int enc_set_configuration(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	const MSVideoConfiguration *vconf = (const MSVideoConfiguration *)arg;
	if (vconf != &d->vconf) memcpy(&d->vconf, vconf, sizeof(MSVideoConfiguration));

	if (d->vconf.required_bitrate > d->vconf.bitrate_limit)
		d->vconf.required_bitrate = d->vconf.bitrate_limit;
	if (d->enc) {
		ms_filter_lock(f);
		apply_bitrate(f);
		if (x264_encoder_reconfig(d->enc, &d->params) != 0) {
			ms_error("x264_encoder_reconfig() failed.");
		}
		ms_filter_unlock(f);
		return 0;
	}

	ms_message("Video configuration set: bitrate=%dbits/s, fps=%f, vsize=%dx%d", d->vconf.required_bitrate, d->vconf.fps, d->vconf.vsize.width, d->vconf.vsize.height);
	return 0;
}

static int enc_set_br(MSFilter *f, void *arg) {
	EncData *d = (EncData *)f->data;
	int br = *(int *)arg;
	if (d->enc != NULL) {
		/* Encoding is already ongoing, do not change video size, only bitrate. */
		d->vconf.required_bitrate = br;
		enc_set_configuration(f,&d->vconf);
	} else {
		MSVideoConfiguration best_vconf = ms_video_find_best_configuration_for_bitrate(d->vconf_list, br, ms_get_cpu_count());
		enc_set_configuration(f, &best_vconf);
	}
	return 0;
}

static int enc_set_fps(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	d->vconf.fps=*(float*)arg;
	enc_set_configuration(f, &d->vconf);
	return 0;
}

static int enc_get_fps(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	*(float*)arg=d->vconf.fps;
	return 0;
}

static int enc_get_vsize(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	*(MSVideoSize*)arg=d->vconf.vsize;
	return 0;
}

static int enc_set_vsize(MSFilter *f, void *arg){
	MSVideoConfiguration best_vconf;
	EncData *d = (EncData *)f->data;
	MSVideoSize *vs = (MSVideoSize *)arg;
	best_vconf = ms_video_find_best_configuration_for_size(d->vconf_list, *vs, ms_get_cpu_count());
	d->vconf.vsize = *vs;
	d->vconf.fps = best_vconf.fps;
	d->vconf.bitrate_limit = best_vconf.bitrate_limit;
	enc_set_configuration(f, &d->vconf);
	return 0;
}

static int enc_add_fmtp(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	const char *fmtp=(const char *)arg;
	char value[12];
	if (fmtp_get_value(fmtp,"packetization-mode",value,sizeof(value))){
		d->mode=atoi(value);
		ms_message("packetization-mode set to %i",d->mode);
	}
	return 0;
}

static int enc_req_vfu(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	d->generate_keyframe=TRUE;
	return 0;
}

static int enc_get_configuration_list(MSFilter *f, void *data) {
	EncData *d = (EncData *)f->data;
	const MSVideoConfiguration **vconf_list = (const MSVideoConfiguration **)data;
	*vconf_list = d->vconf_list;
	return 0;
}


static MSFilterMethod enc_methods[] = {
	{ MS_FILTER_SET_FPS,                       enc_set_fps                },
	{ MS_FILTER_SET_BITRATE,                   enc_set_br                 },
	{ MS_FILTER_GET_BITRATE,                   enc_get_br                 },
	{ MS_FILTER_GET_FPS,                       enc_get_fps                },
	{ MS_FILTER_GET_VIDEO_SIZE,                enc_get_vsize              },
	{ MS_FILTER_SET_VIDEO_SIZE,                enc_set_vsize              },
	{ MS_FILTER_ADD_FMTP,                      enc_add_fmtp               },
	{ MS_FILTER_REQ_VFU,                       enc_req_vfu                },
	{ MS_VIDEO_ENCODER_REQ_VFU,                enc_req_vfu                },
	{ MS_VIDEO_ENCODER_GET_CONFIGURATION_LIST, enc_get_configuration_list },
	{ MS_VIDEO_ENCODER_SET_CONFIGURATION,      enc_set_configuration      },
	{ 0,                                       NULL                       }
};

#ifndef _MSC_VER

static MSFilterDesc x264_enc_desc={
	.id=MS_FILTER_PLUGIN_ID,
	.name="MSX264Enc",
	.text="A H264 encoder based on x264 project",
	.category=MS_FILTER_ENCODER,
	.enc_fmt="H264",
	.ninputs=1,
	.noutputs=1,
	.init=enc_init,
	.preprocess=enc_preprocess,
	.process=enc_process,
	.postprocess=enc_postprocess,
	.uninit=enc_uninit,
	.methods=enc_methods
};

#else

static MSFilterDesc x264_enc_desc={
	MS_FILTER_PLUGIN_ID,
	"MSX264Enc",
	"A H264 encoder based on x264 project",
	MS_FILTER_ENCODER,
	"H264",
	1,
	1,
	enc_init,
	enc_preprocess,
	enc_process,
	enc_postprocess,
	enc_uninit,
	enc_methods
};

#endif

MS2_PUBLIC void libmsx264_init(void){
	ms_filter_register(&x264_enc_desc);
	ms_message("ms264-" VERSION " plugin registered.");
}

