/**
 * @file axidma_transfer.c
 * @date Sunday, April 1, 2021 at 12:23:43 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 * @author Xin Han (hicx)
 *
 * This program performs a simple AXI DMA transfer. It takes the input data,
 * loads it into memory, and then sends it out over the PL fabric. It then
 * receives the data back, and places it into the given output .
 *
 * By default it uses the lowest numbered channels for the transmit and receive,
 * unless overriden by the user. The amount of data transfered is automatically
 * determined from the file size. Unless specified, the output file size is
 * made to be 2 times the input size (to account for creating more data).
 *
 * This program also handles any additional channels that the pipeline
 * on the PL fabric might depend on. It starts up DMA transfers for these
 * pipeline stages, and discards their results.
 *
 * @bug No known bugs.
 **/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include <fcntl.h>    
#include <sys/mman.h>          // Flags for open()
#include <sys/stat.h>           // Open() system call
#include <sys/types.h>          // Types for open()
#include <unistd.h>             // Close() system call
#include <string.h>             // Memory setting and copying
#include <getopt.h>             // Option parsing
#include <errno.h>              // Error codes
#include <string.h>
#include <poll.h>
#include "util.h"               // Miscellaneous utilities
#include "conversion.h"         // Convert bytes to MiBs
#include "axidmaapp.h"          // Interface ot the AXI DMA library
#include <pthread.h>
static unsigned char rbuffer[2048] = {0};
static unsigned char sbuffer[2048] = {0};
#define MAXLENGTH 2048

// Prints the usage for this program
static void print_usage(bool help)
{
    FILE* stream = (help) ? stdout : stderr;

    fprintf(stream, "Usage: axidma_transfer  "
            "[-t <DMA tx channel>] [-r <DMA rx channel>] [-s <Output file size>"
            " | -o <Output file size>].\n");
    if (!help) {
        return;
    }

    // fprintf(stream, "\t<input path>:\t\tThe path to file to send out over AXI "
    //         "DMA to the PL fabric. Can be a relative or absolute path.\n");
    // fprintf(stream, "\t<output path>:\t\tThe path to place the received data "
    //         "from the PL fabric into. Can be a relative or absolute path.\n");
    fprintf(stream, "\t-t <DMA tx channel>:\tThe device id of the DMA channel "
            "to use for transmitting the file. Default is to use the lowest "
            "numbered channel available.\n");
    fprintf(stream, "\t-r <DMA rx channel>:\tThe device id of the DMA channel "
            "to use for receiving the data from the PL fabric. Default is to "
            "use the lowest numbered channel available.\n");
    fprintf(stream, "\t-s <Output file size>:\tThe size of the output file in "
            "bytes. This is an integer value that must be at least the number "
            "of bytes received back. By default, this is the same as the size "
            "of the input file.\n");
    fprintf(stream, "\t-o <Output file size>:\tThe size of the output file in "
            "Mibs. This is a floating-point value that must be at least the "
            "number of bytes received back. By default, this is the same "
            "the size of the input file.\n");
    return;
}

/* Parses the command line arguments overriding the default transfer sizes,
 * and number of transfer to use for the benchmark if specified. */
static int parse_args(int argc, char **argv,  int *input_channel, int *output_channel, int *output_size)
{
    char option;
    int int_arg;
    double double_arg;
    bool o_specified, s_specified;
    int rc;

    // Set the default values for the arguments
    *input_channel = -1;
    *output_channel = -1;
    *output_size = -1;
    o_specified = false;
    s_specified = false;
    rc = 0;

    while ((option = getopt(argc, argv, "t:r:s:o:h")) != (char)-1)
    {
        switch (option)
        {
            // Parse the transmit channel device id
            case 't':
                rc = parse_int(option, optarg, &int_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *input_channel = int_arg;
                break;

            // Parse the receive channel device id
            case 'r':
                rc = parse_int(option, optarg, &int_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *output_channel = int_arg;
                break;

            // Parse the output file size (in bytes)
            case 's':
                rc = parse_int(option, optarg, &int_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *output_size = int_arg;
                s_specified = true;
                break;

            // Parse the output file size (in MiBs)
            case 'o':
                rc = parse_double(option, optarg, &double_arg);
                if (rc < 0) {
                    print_usage(false);
                    return rc;
                }
                *output_size = MIB_TO_BYTE(double_arg);
                o_specified = true;
                break;

            case 'h':
                print_usage(true);
                exit(0);

            default:
                print_usage(false);
                return -EINVAL;
        }
    }

    // If one of -t or -r is specified, then both must be
    if ((*input_channel == -1) ^ (*output_channel == -1)) {
        fprintf(stderr, "Error: Either both -t and -r must be specified, or "
                "neither.\n");
        print_usage(false);
        return -EINVAL;
    }

    // Only one of -s and -o can be specified
    if (s_specified && o_specified) {
        fprintf(stderr, "Error: Only one of -s and -o can be specified.\n");
        print_usage(false);
        return -EINVAL;
    }

    // // Check that there are enough command line arguments
    // if (optind > argc-2) {
    //     fprintf(stderr, "Error: Too few command line arguments.\n");
    //     print_usage(false);
    //     return -EINVAL;
    // }

    // Check if there are too many command line arguments remaining
    if (optind < argc-2) {
        fprintf(stderr, "Error: Too many command line arguments.\n");
        print_usage(false);
        return -EINVAL;
    }

    // Parse out the input and output paths
    // *input_path = argv[optind];
    // *output_path = argv[optind+1];
    return 0;
}

//receive
void *rapidio_taks_rec(int argc, char **argv)
{
    int rc;
    int i;
    int rec_len;
    char *input_path, *output_path;
    axidma_dev_t axidma_dev;
    struct stat input_stat;
    struct dma_transfer trans;
    const array_t *tx_chans, *rx_chans;
    int error;
    int ret;
    

    axidma_config();
    XDma_Out32(map_base0+4,1);

    //sbuffer init
    for(i = 0;i < 2000;i++)
      {
        sbuffer[i]=i;
      }

    // 解析输入参数
    memset(&trans, 0, sizeof(trans));
    if (parse_args(argc, argv, &trans.input_channel,
                   &trans.output_channel, &trans.output_size) < 0) {
        rc = 1;
        goto ret;
    }

    // 初始化AXIDMA设备
    axidma_dev = axidma_init();
    if (axidma_dev == NULL) {
        fprintf(stderr, "Error: Failed to initialize the AXI DMA device.\n");
        rc = 1;
        goto close_output;
    }
     printf("Succeed to initialize the AXI DMA device.\n");

    // // 规定输入输出文件的大小
    // 如果用户没有指定输出大小，则将其设置为默认值
   
    // trans.input_size = 1000;
    // // printf("trans.output_size main1 is %d\n",trans.output_size);
    //  if (trans.output_size == -1) {
    //     trans.output_size = trans.input_size;
    // }
   
    // 如果还没有指定tx和rx通道，则获取收发通道
    tx_chans = axidma_get_dma_tx(axidma_dev);
    // printf("tx_chans->len is%d\n ",tx_chans->len);
    // printf("trans.input_channel is %d\n ",trans.input_channel);
    // printf("tx_chans->data[0] is %d\n ",tx_chans->data[0]);
    if (tx_chans->len < 1) {
        fprintf(stderr, "Error: No transmit channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }
    rx_chans = axidma_get_dma_rx(axidma_dev);
    // printf("rx_chans->len is%d\n ",rx_chans->len);
    //  printf("trans.output_channel is %d\n ",trans.output_channel);
    // printf("rx_chans->data[0] is %d\n ",rx_chans->data[0]);
    if (rx_chans->len < 1) {
        fprintf(stderr, "Error: No receive channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }

    /* 如果用户没有指定通道，我们假设发送和接收通道是编号最低的通道。 */
    if (trans.input_channel == -1 && trans.output_channel == -1) {
        trans.input_channel = tx_chans->data[0];
        trans.output_channel = rx_chans->data[0];
    }
    while(1)
    {
        rec_len = axidma0read(axidma_dev, &trans, rbuffer);
    printf("\nrec_len = 0x%d\n",rec_len);
    for(i = 0;i<(rec_len);i++)
        {
            if(i%16 == 0)
            {
                printf("\n");
            }
            printf("0x%02x ",rbuffer[i]);
        }


      
      }
    
    printf("poll nothing--------------------------\n");
   

   pthread_exit(0);
destroy_axidma:
    axidma_destroy(axidma_dev);
close_output:
    assert(close(trans.output_fd) == 0);
// close_input:
//     assert(close(trans.input_fd) == 0);
ret:
    return rc;
}


/*----------------------------------------------------------------------------
 * Main
 *----------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    int rc;
    int i;
    int rec_len;
    char *input_path, *output_path;
    axidma_dev_t axidma_dev;
    struct stat input_stat;
    struct dma_transfer trans;
    const array_t *tx_chans, *rx_chans;
    int error;
    int ret;

   
    pthread_t rapidio_rid;

    //地址映射，使能读写DMA，单独规定
    axidma_config();
    XDma_Out32(map_base0+4,1);

    //sbuffer init
    for(i = 0;i < 2000;i++)
      {
        sbuffer[i]=i;
      }

    // 解析输入参数
    memset(&trans, 0, sizeof(trans));
    if (parse_args(argc, argv, &trans.input_channel,
                   &trans.output_channel, &trans.output_size) < 0) {
        rc = 1;
        goto ret;
    }

    // 初始化AXIDMA设备
    axidma_dev = axidma_init();
    if (axidma_dev == NULL) {
        fprintf(stderr, "Error: Failed to initialize the AXI DMA device.\n");
        rc = 1;
        goto close_output;
    }
     printf("Succeed to initialize the AXI DMA device.\n");

    // // 规定输入输出文件的大小
    // 如果用户没有指定输出大小，则将其设置为默认值
   
    // trans.input_size = 1000;
    // // printf("trans.output_size main1 is %d\n",trans.output_size);
    //  if (trans.output_size == -1) {
    //     trans.output_size = trans.input_size;
    // }
   
    // 如果还没有指定tx和rx通道，则获取收发通道
    tx_chans = axidma_get_dma_tx(axidma_dev);
    // printf("tx_chans->len is%d\n ",tx_chans->len);
    // printf("trans.input_channel is %d\n ",trans.input_channel);
    // printf("tx_chans->data[0] is %d\n ",tx_chans->data[0]);
    if (tx_chans->len < 1) {
        fprintf(stderr, "Error: No transmit channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }
    rx_chans = axidma_get_dma_rx(axidma_dev);
    // printf("rx_chans->len is%d\n ",rx_chans->len);
    //  printf("trans.output_channel is %d\n ",trans.output_channel);
    // printf("rx_chans->data[0] is %d\n ",rx_chans->data[0]);
    if (rx_chans->len < 1) {
        fprintf(stderr, "Error: No receive channels were found.\n");
        rc = -ENODEV;
        goto destroy_axidma;
    }

    /* 如果用户没有指定通道，我们假设发送和接收通道是编号最低的通道。 */
    if (trans.input_channel == -1 && trans.output_channel == -1) {
        trans.input_channel = tx_chans->data[0];
        trans.output_channel = rx_chans->data[0];
    }
    // trans.input_channel = 0;
    // trans.output_channel = 1;
    printf("AXI DMA File Transfer Info:\n");
    printf("\tTransmit Channel: %d\n", trans.input_channel);
    printf("\tReceive Channel: %d\n", trans.output_channel);
    printf("\tInput Data Size: %.4f MiB\n", BYTE_TO_MIB(trans.input_size));
    printf("\tOutput Data Size: %.4f MiB\n\n", BYTE_TO_MIB(trans.output_size));
     

    // 通过AXI DMA传输文件
    // rc = transfer_file(axidma_dev, &trans, output_path);
    // rc = (rc < 0) ? -rc : 0;
    trans.input_size = 1000;
    trans.output_size = 2048;
    rc = axidma0send(axidma_dev, &trans, sbuffer);
    printf("success send axidma0\n");
    sleep(5);
    trans.input_size = 2000;
     sleep(5);
    rc = axidma0send(axidma_dev, &trans, sbuffer);
    trans.input_size = 1800;
     sleep(5);
    rc = axidma0send(axidma_dev, &trans, sbuffer);
//     rec_len = axidma0read(axidma_dev, &trans, rbuffer);
//     printf("\nrec_len = 0x%d\n",rec_len);
//     for(i = 0;i<(rec_len);i++)
//         {
//             if(i%16 == 0)
//             {
//                 printf("\n");
//             }
//             printf("0x%02x ",rbuffer[i]);
//         }

//     sleep(3);

//      trans.input_size = 2000;
//      trans.output_size = MAXLENGTH;
    
//     rc = axidma0send(axidma_dev, &trans, sbuffer);
//     printf("success send axidma0\n");
//     rec_len = axidma0read(axidma_dev, &trans, rbuffer);
//     printf("\nrec_len = 0x%d\n",rec_len);
//     for(i = 0;i<(rec_len);i++)
//         {
//             if(i%16 == 0)
//             {
//                 printf("\n");
//             }
//             printf("0x%02x ",rbuffer[i]);
//         }




// sleep(3);

//     trans.input_size = 1500;
//     rc = axidma0send(axidma_dev, &trans, sbuffer);
//      printf("success send axidma0\n");
//     rec_len = axidma0read(axidma_dev, &trans, rbuffer);
//     printf("\nrec_len = 0x%d\n",rec_len);
//     for(i = 0;i<(rec_len);i++)
//         {
//             if(i%16 == 0)
//             {
//                 printf("\n");
//             }
//             printf("0x%02x ",rbuffer[i]);
//         }

// sleep(3);

//      trans.input_size = 1000;
//     rc = axidma0send(axidma_dev, &trans, sbuffer);
//      printf("success send axidma0\n");
   
    
error=pthread_create(&rapidio_rid, NULL, &rapidio_taks_rec,NULL);
    if(error != 0)
    {
        printf("pthreadrx_create fail\n");
        return -1;
    }
pthread_detach(rapidio_rid);
   rc = (rc < 0) ? -rc : 0;    
destroy_axidma:
    axidma_destroy(axidma_dev);
close_output:
    assert(close(trans.output_fd) == 0);
// close_input:
//     assert(close(trans.input_fd) == 0);
ret:
    return rc;
}
