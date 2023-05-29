#include <linux/videodev2.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct _buffer {
  void *start;
  size_t length;
};

/*	number of allocated buffers	*/
static unsigned int n_buffers;
/*	number of frames to capture	*/
static unsigned int n_frames;
/*	index of current frame	*/
static unsigned int count;
/*	device file descriptor	*/
static int fd;
/*	time interval	to sleep (s)	*/
static unsigned int interval=1;
/*	*/
struct _buffer *buffers;
/*	output directory (relative path)	*/
static const char * out_dir="frames/";

static void process_image(const void *p, int size)
{
		char path[0x100];
		sprintf(path,"%s/frame%u.bin",out_dir,count);
		FILE * fp=fopen(path,"w");
    if (!fp){
    	fprintf(stderr,"unable to open output file (%s)\n",path);
    	/*	not critical	*/
    	return;
    }
    fwrite(p, size, 1, fp);
    fflush(fp);
    fclose(fp);
}

static int read_frame(){
	struct v4l2_buffer buf;
	memset(&buf,0,sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (-1 == ioctl(fd, VIDIOC_DQBUF, &buf)) {
    switch (errno) {
		  case EAGAIN:{
		  	return 0;
		  }
		  case EIO:{
		    /* Could ignore EIO, see spec. */
		    fprintf(stderr,"WARNING (VIDIOC_DQBUF): EIO is ignored\n");  
		    /* fall through */
		    return 0;
		  }
		  default:{
		    perror("VIDIOC_DQBUF");
		    exit(EXIT_FAILURE);
		  }
  	}
  }
  assert(buf.index < n_buffers);
  process_image(buffers[buf.index].start, buf.bytesused);
  /*	re-queueing	*/
  if (-1 == ioctl(fd, VIDIOC_QBUF, &buf)){
		perror("VIDIOC_QBUF");
		exit(EXIT_FAILURE);
	}
	return 1;          
}

static void loop(){
  for (;count<n_frames;count++) {
  	/*	infinite loop to read the next frame	*/
    for (;;) {
      fd_set fds;
      struct timeval tv;
      int r;

      FD_ZERO(&fds);
      FD_SET(fd, &fds);

      /* Timeout. */
      tv.tv_sec = 0;
      tv.tv_usec = 0;

			/*	sleep for specified interval before reading next frame	*/
			sleep(interval);
      r = select(fd + 1, &fds, NULL, NULL, &tv);
      if (-1 == r) {
              if (EINTR == errno)
                      continue;
              perror("select:");
              exit(EXIT_FAILURE);
      }
      if (0 == r) {
              perror("select timeout:");
              exit(EXIT_FAILURE);
      }
      if (read_frame())
              break;
      /* EAGAIN - continue select loop. */
    }
  }
}

int main(int argc,char * argv[]){
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_format fmt;
	/*	used to start capturing	*/
	enum v4l2_buf_type type;
	unsigned int i;
	
	
	fd=open("/dev/video2",O_RDWR);
	if(fd<0){
		perror("open:");
		return -1;
	}

	/*	request to allocate buffers	*/
	memset(&reqbuf, 0, sizeof(reqbuf));
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.count = 20;
	if (-1 == ioctl (fd, VIDIOC_REQBUFS, &reqbuf)) {
		  if (errno == EINVAL)
		      printf("Video capturing or mmap-streaming is not supported\\n");
		  else
		      perror("VIDIOC_REQBUFS");

		  exit(EXIT_FAILURE);
	}
	/* We want at least five buffers. */
	if (reqbuf.count < 5) {
		  /* You may need to free the buffers here. */
		  printf("Not enough buffer memory\\n");
		  exit(EXIT_FAILURE);
	}else{
		/*	save number of allocated buffers	*/
		n_buffers=reqbuf.count;
		n_frames=reqbuf.count;
	}
	
	/*	request for supported image format	*/
	memset(&fmt,0,sizeof(struct v4l2_format));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(-1==ioctl(fd,VIDIOC_G_FMT,&fmt)){
		perror("VIDIOC_G_FMT:");
		return -1;
	}
	/*	logging	*/
	fprintf(stdout,"v4l2_pix_format: width=%u, height=%u, pixelformat=%u, colorspace=%u, bytesperline=%u\n",
		fmt.fmt.pix.width,fmt.fmt.pix.height,fmt.fmt.pix.pixelformat,fmt.fmt.pix.colorspace,fmt.fmt.pix.bytesperline);
	
	buffers = calloc(n_buffers, sizeof(*buffers));
	assert(buffers != NULL);
	for (i = 0; i < n_buffers; i++) {
		  struct v4l2_buffer buffer;
		  memset(&buffer, 0, sizeof(buffer));
		  buffer.type = reqbuf.type;
		  buffer.memory = V4L2_MEMORY_MMAP;
		  buffer.index = i;
		  if (-1 == ioctl (fd, VIDIOC_QUERYBUF, &buffer)) {
		      perror("VIDIOC_QUERYBUF");
		      exit(EXIT_FAILURE);
		  }
			/* Remember for munmap(). All buffers have the same length. */
		  buffers[i].length = buffer.length; 
		  /*	mapping to process memory	*/
		  buffers[i].start = mmap(NULL, buffer.length,
		              PROT_READ | PROT_WRITE, /* recommended */
		              MAP_SHARED,             /* recommended */
		              fd, buffer.m.offset);
		  if (MAP_FAILED == buffers[i].start) {
		      /* If you do not exit here you should unmap() and free()
		         the buffers mapped so far. */
		      perror("mmap");
		      exit(EXIT_FAILURE);
		  }
	}
	/*	Enqueue allocated buffers	*/
	for(i=0;i<n_buffers;i++){
		struct v4l2_buffer buf;
		memset(&buf,0,sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		/*	enqueue the buffer	*/
		if (-1 == ioctl(fd, VIDIOC_QBUF, &buf)){
    	perror("VIDIOC_QBUF:");
    	exit(EXIT_FAILURE);
    }
	}
	/*	Start capturing	*/
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == ioctl(fd, VIDIOC_STREAMON, &type)){
  	perror("VIDIOC_STREAMON:");
  	exit(EXIT_FAILURE);
  }
 	loop();               
	/* Cleanup. */
	for (i = 0; i < reqbuf.count; i++)
		  munmap(buffers[i].start, buffers[i].length);	  
	return 0;
}
