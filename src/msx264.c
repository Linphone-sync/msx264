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


#include <x264.h>

#define RC_MARGIN 10000 /*bits per sec*/

typedef struct _EncData{
	x264_t *enc;
	MSVideoSize vsize;
	int bitrate;
	float fps;
	int mode;
	uint64_t framenum;
	Rfc3984Context *packer;
	int keyframe_int;
	bool_t generate_keyframe;
}EncData;


static void enc_init(MSFilter *f){
	EncData *d=ms_new(EncData,1);
	d->enc=NULL;
	d->bitrate=384000;
	d->vsize=MS_VIDEO_SIZE_CIF;
	d->fps=30;
	d->keyframe_int=10; /*10 seconds */
	d->mode=0;
	d->framenum=0;
	d->generate_keyframe=FALSE;
	d->packer=NULL;
	f->data=d;
}

static void enc_uninit(MSFilter *f){
	EncData *d=(EncData*)f->data;
	ms_free(d);
}

static void enc_preprocess(MSFilter *f){
	EncData *d=(EncData*)f->data;
	x264_param_t params;
	float bitrate;
	
	d->packer=rfc3984_new();
	rfc3984_set_mode(d->packer,d->mode);
	rfc3984_enable_stap_a(d->packer,FALSE);
	
	x264_param_default(&params);
	params.i_threads=1;
	params.i_sync_lookahead=0;
	params.i_width=d->vsize.width;
	params.i_height=d->vsize.height;
	params.i_fps_num=(int)d->fps;
	params.i_fps_den=1;
	params.i_slice_max_size=ms_get_payload_max_size()-100; /*-100 security margin*/
	params.i_level_idc=13;

	bitrate=(float)d->bitrate*0.92;
	if (bitrate>RC_MARGIN)
		bitrate-=RC_MARGIN;
	
	params.rc.i_rc_method = X264_RC_ABR;
	params.rc.i_bitrate=(int)(bitrate/1000);
	params.rc.f_rate_tolerance=0.1;
	params.rc.i_vbv_max_bitrate=(int) ((bitrate+RC_MARGIN/2)/1000);
	params.rc.i_vbv_buffer_size=params.rc.i_vbv_max_bitrate;
	params.rc.f_vbv_buffer_init=0.5;
	params.rc.i_lookahead=0;
	/*enable this by config ?*/
	/*
	params.i_keyint_max = (int)d->fps*d->keyframe_int;
	params.i_keyint_min = (int)d->fps;
	*/
	params.b_repeat_headers=1;
	params.b_annexb=0;

	//these parameters must be set so that our stream is baseline
	params.analyse.b_transform_8x8 = 0;
	params.b_cabac = 0;
	params.i_cqm_preset = X264_CQM_FLAT;
	params.i_bframe = 0;
	params.analyse.i_weighted_pred = X264_WEIGHTP_NONE;
	
	d->enc=x264_encoder_open(&params);
	if (d->enc==NULL) ms_error("Fail to create x264 encoder.");
	d->framenum=0;
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
	ms_queue_init(&nalus);
	while((im=ms_queue_get(f->inputs[0]))!=NULL){
		if (ms_yuv_buf_init_from_mblk(&pic,im)==0){
			x264_picture_t xpic;
			x264_picture_t oxpic;
			x264_nal_t *xnals=NULL;
			int num_nals=0;

			/*send I frame 2 seconds and 4 seconds after the beginning */
			if (d->framenum==(int)d->fps*2 || d->framenum==(int)d->fps*4)
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
				rfc3984_pack(d->packer,&nalus,f->outputs[0],ts);
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

static int enc_set_br(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	d->bitrate=*(int*)arg;

	if (d->bitrate>=1024000){
		d->vsize=MS_VIDEO_SIZE_VGA;
		d->fps=25;
	}else if (d->bitrate>=512000){
		d->vsize=MS_VIDEO_SIZE_VGA;
		d->fps=25;
        }else if (d->bitrate>=384000){
		d->vsize=MS_VIDEO_SIZE_CIF;
		d->fps=25;
	}else if (d->bitrate>=256000){
		d->vsize=MS_VIDEO_SIZE_CIF;
		d->fps=15;
	}else if (d->bitrate>=128000){
		d->vsize=MS_VIDEO_SIZE_CIF;
		d->fps=15;
	}else if (d->bitrate>=64000){
		d->vsize=MS_VIDEO_SIZE_CIF;
		d->fps=10;
	}else if (d->bitrate>=32000){
		d->vsize=MS_VIDEO_SIZE_QCIF;
		d->fps=10;
	}else{
		d->vsize=MS_VIDEO_SIZE_QCIF;
		d->fps=5;
	}
	ms_message("bitrate set to %i",d->bitrate);
	return 0;
}

static int enc_set_fps(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	d->fps=*(float*)arg;
	return 0;
}

static int enc_get_fps(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	*(float*)arg=d->fps;
	return 0;
}

static int enc_get_vsize(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	*(MSVideoSize*)arg=d->vsize;
	return 0;
}

static int enc_set_vsize(MSFilter *f, void *arg){
	EncData *d=(EncData*)f->data;
	d->vsize=*(MSVideoSize*)arg;
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


static MSFilterMethod enc_methods[]={
	{	MS_FILTER_SET_FPS	,	enc_set_fps	},
	{	MS_FILTER_SET_BITRATE	,	enc_set_br	},
	{	MS_FILTER_GET_FPS	,	enc_get_fps	},
	{	MS_FILTER_GET_VIDEO_SIZE,	enc_get_vsize	},
	{	MS_FILTER_SET_VIDEO_SIZE,	enc_set_vsize	},
	{	MS_FILTER_ADD_FMTP	,	enc_add_fmtp	},
	{	MS_FILTER_REQ_VFU	,	enc_req_vfu	},
	{	0	,			NULL		}
};

static MSFilterDesc x264_enc_desc={
	.id=MS_FILTER_PLUGIN_ID,
	.name="MSX264Enc",
	.text="A H264 encoder based on x264 project (with multislicing enabled)",
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

void libmsx264_init(void){
	ms_filter_register(&x264_enc_desc);
	ms_message("ms264-" VERSION " plugin registered.");
}

