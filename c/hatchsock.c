#include "includes.h"
#define SOCKDEBUG 0

/**
 * Server. Listens for messages containing machine-code, executes
 * them, and returns the resulting registers state.
 **/

#define PORT 9999
#define TRANSMISSION_SIZE 512

#define RET(x) (x == 0xC3)
#define READY(x) (x < 0)

#define DUMP 1
#define MAX_CODE_SIZE 0x10000
#define SEXP_LENGTH 256

#define BREAKPOINT_OPCODE 0xCC

#define ARM_WORD_LEN 4
#define X86_NOP 0x90

#define ARM_NOP "\x00\x00\x00\x00"


#define BAREMETAL 0

// marks end of code transmission

u8 *armrets[] = {
  "\x0e\xf0\xa0\xe1", // mov  pc, lr
};

u32 armrets_len = 1;


/**
 * A bit of cut-and-pasta from Hacking: The Art of Exploitation
 **/
void fatal(char *message){
  char error_message[100];
  strcpy(error_message, "[!!] Fatal Error ");
  strncat(error_message, message, 83);
  perror(error_message);
  exit(-1);
}

/************************************************************/

u32 validate(char *header){

  return 1;
}


/* compare with arm-pop-pc-p in roper.lisp */
u32 arm_pop_pc_p(u8 *word){
  return ((word[3] == 0xe8) &&          // is it a pop?
          (word[2] == 0xbd) &&
          (word[1] & (1 << 7) != 0));      // into register 15 (7+8)?
}
u32 arm_return_p(u8 *word){
  u32 i;
  if (arm_pop_pc_p(word))
    return 1;
  for (i = 0; i < armrets_len; i++){
    if (!memcmp(word, armrets[i], ARM_WORD_LEN)){
      return 1;
      break;
    }
  }
  return 0;
}


/* u32 parse_header(u8 *recvbuffer){ */
  
/* } */


/**
 * Returns next free position in codebuffer, or -1 * that position
 * if a return instruction has been received.
 **/
u32 codecopy(unsigned char *codebuffer, unsigned char *recvbuffer,
             u32 codelength, u32 recvlength, uc_arch arch){
  u32 i;
  /* Insert a breakpoint at the beginning of the code */
  /* it's more efficient to do this here, than in hatch_code */
  /*
    if (!codelength){
    codebuffer[0] = 0xCC;
    codebuffer[1] = 0x03;
    codelength = 2;
    if (SOCKDEBUG) printf("Added breakpoint at head.\n");
    }
  */
  
  if (arch == UC_ARCH_X86) { // check to see this is default for bare metal mode
    for (i = 0; i < recvlength; i ++){
      codebuffer[codelength++] =
        (RET(recvbuffer[i]))? 0x90 : recvbuffer[i];
      /* if (RET(recvbuffer[i])){ */
      /*   //        codelength = -(codelength); */
      /*   break; */      /* } */
    }
  } else if (arch == UC_ARCH_ARM) {
    u8 lastword[4] = {0,0,0,0};
    
    for (i = 0; i < recvlength; i ++){
      lastword[i%4] = recvbuffer[i];
      arm_return_p(lastword);
      codebuffer[codelength++] = recvbuffer[i];
      /* if (arm_return_p(lastword)){ */
      /*   //        memcpy(codebuffer+(codelength-4), ARM_NOP, ARM_WORD_LEN);  */
      /*   //        codelength = -(codelength); */
      /*   break; */
      /* } */
    }
  }  
  return codelength;
}
    

  

u32 lisp_encode(unsigned char *vector, char *sexp){
  u32 vptr=0, sptr, length=0;
  // maximum length for sexp is 1 + 2 + (16+2)*7 + 7
  // #, parens, seven 64bit hex numbers, prefixed by #x, spaces
  // grand total of 135, plus a null character, so 137
  // vector is SYSREG_BYTES long. Bump it up to 256. More than we need
  memset(sexp, 0, SEXP_LENGTH);
    
  length += sprintf(sexp+length, "#(");

  // we should do something about this magic #. parameterize.
  // it's the # of registers we're tracking, btw. 
  for (vptr = 0; vptr < 64; vptr += sizeof(long int)) {
    length += sprintf(sexp+length, "#x%llx ",
                      bytes_to_integer(vector + vptr));
  }
  length --;
  length += sprintf(sexp+length, ")\n\0");

  if (SOCKDEBUG) printf("SEXP: %s\n", sexp);
  
  return length;
}

#define SET_BY_CLIENT -1
/******************************************************************/
/* Some functional macros for quickly parsing the packet's header */
/******************************************************************/
#define BAREMETAL(x) (0x01 & x[0])
#define RESET(x) (0x02 & x[0]) 
#define RESPOND(x) (0x04 & x[0])
#define ARCHFLAG(x) ((0xF0 & x[0])? UC_ARCH_ARM : UC_ARCH_X86)
#define EXPECT(x) ((x[1]) | ( (x[2] << 8)))
#define STARTAT(x)  ((0xFF & x[3]) |  ((0xFF & x[4]) << 8) | \
                     ((0xFF & x[5]) << 16) | ((0xFF & x[6]) << 24)) 
#define HEADERLENGTH 7

/******************************************************************/

u32 listen_for_code(u32 port){

  u32 sockfd, new_sockfd, yes=1, recvlength=1;
  uc_arch arch;
  socklen_t sin_size;
  char buffer[TRANSMISSION_SIZE];
  struct sockaddr_in srv_addr, cli_addr;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    fatal("in socket");

  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(u32)) == -1)
    fatal("setting socket option SO_REUSEADDR");

  srv_addr.sin_family = AF_INET;   // host byte order
  srv_addr.sin_port = htons(port); // short, network byte order
  srv_addr.sin_addr.s_addr = 0;    // automatically fill with my ip address

  memset(&(srv_addr.sin_zero), '\0', 8); // zero out the rest of the struct

  if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(struct sockaddr)) == -1)
    fatal("binding to socket");

  if (listen(sockfd, 5) == -1)
    fatal("listening on socket");

  /**
   * Main loop
   **/

  unsigned char *codebuffer;
  unsigned char *result;
  char *sexp;
  u32 codelength, actual_sexp_length;
  
  codebuffer = malloc(MAX_CODE_SIZE);
  result = malloc(16 * sizeof(u32)); // make this more flexible
  sexp = malloc(SEXP_LENGTH);
  
  while (1) {
    sin_size = sizeof(struct sockaddr_in);
    
    if ((new_sockfd =
         accept(sockfd, (struct sockaddr *) &cli_addr, &sin_size))
        == -1)
      fatal("accepting connection");

    if (SOCKDEBUG) printf("SERVER: ACCEPTED CONNECTION "
                          "FROM %s PORT %d\n",
                          inet_ntoa(cli_addr.sin_addr),
                          ntohs(cli_addr.sin_port));

    recvlength = recv(new_sockfd, &buffer, TRANSMISSION_SIZE, 0);
    
    /** 
     * Clean the buffers
     **/
    memset(codebuffer, 0, MAX_CODE_SIZE);
    //    memset(result, 0, SYSREG_BYTES);
    memset(sexp, 0, SEXP_LENGTH);
    
    codelength = 0;
    u32 offset = 0;
    u8 baremetal = 0;
    //    u32 params = 0;
    u32 startat = 0;
    u16 expect = 0;
    u8 respond = 0;
    u8 reset = 0; // treat as boolean
    
    baremetal = SET_BY_CLIENT;
    while (recvlength > 0) {

      if(!codelength){
        if (SOCKDEBUG){
          printf("HEADER: %d bytes\n", HEADERLENGTH);
          fdump(stdout, buffer, HEADERLENGTH);
        }

        /******************************/
        /* Extract header information */
        /******************************/
        baremetal = BAREMETAL(buffer);
        arch = ARCHFLAG(buffer);
        expect = EXPECT(buffer);
        reset = RESET(buffer);
        respond = RESPOND(buffer);
        startat = (u32) STARTAT(buffer);
        /*****************************/
        if (reset)
          memset(result, 0, SYSREG_BYTES);
        
        if (SOCKDEBUG)
          printf("baremetal = %s\narch = %s\nexpect = %d\n"
                 "reset = %s\nstartat = %x\n",
                 baremetal? "yes":"no",
                 arch == UC_ARCH_ARM? "arm":"x86",
                 expect, reset? "yes" : "no",
                 startat);
        
        //if (!codelength && baremetal < 0){
        /* The lower nibble of the first byte sets the bare metal
         * vs virtual option (1 for bare metal, 0 for virtual).
         * The upper nibble sets the architecture, when virtual. 
         * Currently: 1 for ARM, 0 for X86;
         */         

        recvlength -= 3;
        offset = 3;
      }
      
      codelength = codecopy(codebuffer, buffer + offset,
      codelength, recvlength, arch);

      if (SOCKDEBUG)
        printf("code length = %d\n", (codelength));

      if (codelength >= expect) { //(READY(codelength)){
        /*
         * todo: load the register seed from a file. 
         * this file will be written in the code analysis stage
         * probably from the lispy side of things
         */
        //codelength *= -1;
        if (SOCKDEBUG)
          fdump(stdout, codebuffer, codelength);
        
        /*************************************************/
        /* This is where the code actually gets launched */
        /*************************************************/
        // for debugging //
        if (SOCKDEBUG){
          printf("REGISTERS GOING IN: \n");
          lisp_encode(result, sexp);
        }
        if (baremetal) {
          if (SOCKDEBUG)
            printf("Running on bare metal.\n");
          hatch_code(codebuffer, codelength, NULL, result);
        } else {
          if (SOCKDEBUG)
            printf("Running in virtual environment.\n");
          em_code(codebuffer, codelength, startat, result, arch);
        }
        /*************************************************
         em_code takes seed and result as same pointer.
         hatch_code should be updated to do this too. 
        *************************************************/
        if (respond) {
          actual_sexp_length = lisp_encode(result, sexp);
          send(new_sockfd, sexp, actual_sexp_length, 0);
        } else {
          send(new_sockfd, "Ok", 2, 0);
        }
        break;
      } else {      
        recvlength = recv(new_sockfd, &buffer, TRANSMISSION_SIZE, 0);
      }
    }

    close(new_sockfd);
  }
  printf("Now we are at the end...\n");
  free(codebuffer);
  free(result);
  free(sexp);
}

u32 main(u32 argc, char **argv){
  /*
   * TODO: parse command line options to select architecture
   * and virtualization vs baremetal options
   */
  char opt;
  u32 port = 9999;
  if (argc < 2)
    goto noopts;
  while ((opt = getopt(argc, argv, "p:v:")) != -1){
    switch (opt) {
    case 'v':
      printf("verification not yet implemented.\n");
      break;
    case 'p':
      sscanf(optarg, "%d",&port);
      break;
    case 'h':
    default:
      fprintf(stderr,"TODO: write help documentation.\n");
    }
  }
  noopts:
            
  
  printf("************************************************************\n"
         "*                     READY TO SERVE...                    *\n"
         "* Send machine code to be executed to port %4d.           *\n"
         "* This is not a secure service. Run this on an insecure    *\n"
         "* network, and you *will* be pwned.                        *\n"
         "************************************************************\n", port);
  listen_for_code(port);
  
  return 0;
}


