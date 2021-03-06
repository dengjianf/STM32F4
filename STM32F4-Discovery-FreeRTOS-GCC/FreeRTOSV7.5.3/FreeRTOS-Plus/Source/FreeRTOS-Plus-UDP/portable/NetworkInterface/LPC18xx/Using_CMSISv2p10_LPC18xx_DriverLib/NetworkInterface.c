/*
 * FreeRTOS+UDP V1.0.1 (C) 2013 Real Time Engineers ltd.
 * All rights reserved
 *
 * This file is part of the FreeRTOS+UDP distribution.  The FreeRTOS+UDP license
 * terms are different to the FreeRTOS license terms.
 *
 * FreeRTOS+UDP uses a dual license model that allows the software to be used
 * under a pure GPL open source license (as opposed to the modified GPL license
 * under which FreeRTOS is distributed) or a commercial license.  Details of
 * both license options follow:
 *
 * - Open source licensing -
 * FreeRTOS+UDP is a free download and may be used, modified, evaluated and
 * distributed without charge provided the user adheres to version two of the
 * GNU General Public License (GPL) and does not remove the copyright notice or
 * this text.  The GPL V2 text is available on the gnu.org web site, and on the
 * following URL: http://www.FreeRTOS.org/gpl-2.0.txt.
 *
 * - Commercial licensing -
 * Businesses and individuals that for commercial or other reasons cannot comply
 * with the terms of the GPL V2 license must obtain a commercial license before 
 * incorporating FreeRTOS+UDP into proprietary software for distribution in any 
 * form.  Commercial licenses can be purchased from http://shop.freertos.org/udp 
 * and do not require any source files to be changed.
 *
 * FreeRTOS+UDP is distributed in the hope that it will be useful.  You cannot
 * use FreeRTOS+UDP unless you agree that you use the software 'as is'.
 * FreeRTOS+UDP is provided WITHOUT ANY WARRANTY; without even the implied
 * warranties of NON-INFRINGEMENT, MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. Real Time Engineers Ltd. disclaims all conditions and terms, be they
 * implied, expressed, or statutory.
 *
 * 1 tab == 4 spaces!
 *
 * http://www.FreeRTOS.org
 * http://www.FreeRTOS.org/udp
 *
 */

/* Standard includes. */
#include <stdint.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+UDP includes. */
#include "FreeRTOS_UDP_IP.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_Sockets.h"
#include "NetworkBufferManagement.h"

/* Driver includes. */
#include "lpc18xx_emac.h"

/* Demo includes. */
#include "NetworkInterface.h"

#ifndef configNUM_RX_ETHERNET_DMA_DESCRIPTORS
	#error configNUM_RX_ETHERNET_DMA_DESCRIPTORS must be defined in FreeRTOSConfig.h to set the number of RX DMA descriptors
#endif

#ifndef configNUM_TX_ETHERNET_DMA_DESCRIPTORS
	#error configNUM_TX_ETHERNET_DMA_DESCRIPTORS must be defined in FreeRTOSConfig.h to set the number of TX DMA descriptors
#endif

/* If a packet cannot be sent immediately then the task performing the send
operation will be held in the Blocked state (so other tasks can execute) for
niTX_BUFFER_FREE_WAIT ticks.  It will do this a maximum of niMAX_TX_ATTEMPTS
before giving up. */
#define niTX_BUFFER_FREE_WAIT	( ( portTickType ) 2UL / portTICK_RATE_MS )
#define niMAX_TX_ATTEMPTS		( 5 )

/*-----------------------------------------------------------*/

/*
 * A deferred interrupt handler task that processes received frames.
 */
static void prvEMACDeferredInterruptHandlerTask( void *pvParameters );

/*-----------------------------------------------------------*/

/* The queue used to communicate Ethernet events to the IP task. */
extern xQueueHandle xNetworkEventQueue;

/* The semaphore used to wake the deferred interrupt handler task when an Rx
interrupt is received. */
static xSemaphoreHandle xEMACRxEventSemaphore = NULL;

/*-----------------------------------------------------------*/

portBASE_TYPE xNetworkInterfaceInitialise( void )
{
EMAC_CFG_Type Emac_Config;
portBASE_TYPE xReturn;
extern uint8_t ucMACAddress[ 6 ];

	Emac_Config.pbEMAC_Addr = ucMACAddress;
	xReturn = EMAC_Init( &Emac_Config );

	if( xReturn == pdPASS )
	{
		LPC_ETHERNET->DMA_INT_EN =  DMA_INT_NOR_SUM | DMA_INT_RECEIVE;

		/* Create the event semaphore if it has not already been created. */
		if( xEMACRxEventSemaphore == NULL )
		{
			vSemaphoreCreateBinary( xEMACRxEventSemaphore );
			#if ipconfigINCLUDE_EXAMPLE_FREERTOS_PLUS_TRACE_CALLS == 1
			{
				/* If the trace recorder code is included name the semaphore for
				viewing in FreeRTOS+Trace. */
				vTraceSetQueueName( xEMACRxEventSemaphore, "MAC_RX" );
			}
			#endif /*  ipconfigINCLUDE_EXAMPLE_FREERTOS_PLUS_TRACE_CALLS == 1 */
		}

		configASSERT( xEMACRxEventSemaphore );

		/* The Rx deferred interrupt handler task is created at the highest
		possible priority to ensure the interrupt handler can return directly to
		it no matter which task was running when the interrupt occurred. */
		xTaskCreate( 	prvEMACDeferredInterruptHandlerTask,		/* The function that implements the task. */
						( const signed char * const ) "MACTsk",
						configMINIMAL_STACK_SIZE,	/* Stack allocated to the task (defined in words, not bytes). */
						NULL, 						/* The task parameter is not used. */
						configMAX_PRIORITIES - 1, 	/* The priority assigned to the task. */
						NULL );						/* The handle is not required, so NULL is passed. */

		/* Enable the interrupt and set its priority as configured.  THIS
		DRIVER REQUIRES configMAC_INTERRUPT_PRIORITY TO BE DEFINED, PREFERABLY
		IN FreeRTOSConfig.h. */
		NVIC_SetPriority( ETHERNET_IRQn, configMAC_INTERRUPT_PRIORITY );
		NVIC_EnableIRQ( ETHERNET_IRQn );
	}

	return xReturn;
}
/*-----------------------------------------------------------*/

portBASE_TYPE xNetworkInterfaceOutput( xNetworkBufferDescriptor_t * const pxNetworkBuffer )
{
portBASE_TYPE xReturn = pdFAIL;
int32_t x;

	/* Attempt to obtain access to a Tx descriptor. */
	for( x = 0; x < niMAX_TX_ATTEMPTS; x++ )
	{
		if( EMAC_CheckTransmitIndex() == TRUE )
		{
			/* Assign the buffer being transmitted to the Tx descriptor. */
			EMAC_SetNextPacketToSend( pxNetworkBuffer->pucEthernetBuffer );

			/* The EMAC now owns the buffer and will free it when it has been
			transmitted.  Set pucBuffer to NULL to ensure the buffer is not
			freed when the network buffer structure is returned to the pool
			of network buffers. */
			pxNetworkBuffer->pucEthernetBuffer = NULL;

			/* Initiate the Tx. */
			EMAC_StartTransmitNextBuffer( pxNetworkBuffer->xDataLength );
			iptraceNETWORK_INTERFACE_TRANSMIT();

			/* The Tx has been initiated. */
			xReturn = pdPASS;

			break;
		}
		else
		{
			iptraceWAITING_FOR_TX_DMA_DESCRIPTOR();
			vTaskDelay( niTX_BUFFER_FREE_WAIT );
		}
	}

	/* Finished with the network buffer. */
	vNetworkBufferRelease( pxNetworkBuffer );

	return xReturn;
}
/*-----------------------------------------------------------*/

void ETH_IRQHandler( void )
{
uint32_t ulInterruptCause;

	ulInterruptCause = LPC_ETHERNET->DMA_STAT ;

	/* Clear the interrupt. */
	LPC_ETHERNET->DMA_STAT |= ( DMA_INT_NOR_SUM | DMA_INT_RECEIVE );

	/* Clear fatal error conditions.  NOTE:  The driver does not clear all
	errors, only those actually experienced.  For future reference, range
	errors are not actually errors so can be ignored. */
	if( ( ulInterruptCause & ( 1 << 13 ) ) != 0U )
	{
		LPC_ETHERNET->DMA_STAT |= ( 1 << 13 );
	}

	/* Unblock the deferred interrupt handler task if the event was an Rx. */
	if( ( ulInterruptCause & DMA_INT_RECEIVE ) != 0UL )
	{
		xSemaphoreGiveFromISR( xEMACRxEventSemaphore, NULL );
	}

	/* ulInterruptCause is used for convenience here.  A context switch is
	wanted, but coding portEND_SWITCHING_ISR( 1 ) would likely result in a
	compiler warning. */
	portEND_SWITCHING_ISR( ulInterruptCause );
}
/*-----------------------------------------------------------*/

static void prvEMACDeferredInterruptHandlerTask( void *pvParameters )
{
xNetworkBufferDescriptor_t *pxNetworkBuffer;
xIPStackEvent_t xRxEvent = { eEthernetRxEvent, NULL };

	( void ) pvParameters;
	configASSERT( xEMACRxEventSemaphore );

	for( ;; )
	{
		/* Wait for the EMAC interrupt to indicate that another packet has been
		received.  The while() loop is only needed if INCLUDE_vTaskSuspend is
		set to 0 in FreeRTOSConfig.h.  If INCLUDE_vTaskSuspend is set to 1
		then portMAX_DELAY would be an indefinite block time and
		xSemaphoreTake() would only return when the semaphore was actually
		obtained. */
		while( xSemaphoreTake( xEMACRxEventSemaphore, portMAX_DELAY ) == pdFALSE );

		/* At least one packet has been received. */
		while( EMAC_CheckReceiveIndex() != FALSE )
		{
			/* The buffer filled by the DMA is going to be passed into the IP
			stack.  Allocate another buffer for the DMA descriptor. */
			pxNetworkBuffer = pxNetworkBufferGet( ipTOTAL_ETHERNET_FRAME_SIZE, ( portTickType ) 0 );

			if( pxNetworkBuffer != NULL )
			{
				/* Swap the buffer just allocated and referenced from the
				pxNetworkBuffer with the buffer that has already been filled by
				the DMA.  pxNetworkBuffer will then hold a reference to the
				buffer that already contains the data without any data having
				been copied between buffers. */
				EMAC_NextPacketToRead( pxNetworkBuffer );

				#if ipconfigETHERNET_DRIVER_FILTERS_FRAME_TYPES == 1
				{
					if( pxNetworkBuffer->xDataLength > 0 )
					{
						/* If the frame would not be processed by the IP stack then
						don't even bother sending it to the IP stack. */
						if( eConsiderFrameForProcessing( pxNetworkBuffer->pucEthernetBuffer ) != eProcessBuffer )
						{
							pxNetworkBuffer->xDataLength = 0;
						}
					}
				}
				#endif

				if( pxNetworkBuffer->xDataLength > 0 )
				{
					/* Store a pointer to the network buffer structure in the
					padding	space that was left in front of the Ethernet frame.
					The pointer	is needed to ensure the network buffer structure
					can be located when it is time for it to be freed if the
					Ethernet frame gets	used as a zero copy buffer. */
					*( ( xNetworkBufferDescriptor_t ** ) ( ( pxNetworkBuffer->pucEthernetBuffer - ipBUFFER_PADDING ) ) ) = pxNetworkBuffer;

					/* Data was received and stored.  Send it to the IP task
					for processing. */
					xRxEvent.pvData = ( void * ) pxNetworkBuffer;
					if( xQueueSendToBack( xNetworkEventQueue, &xRxEvent, ( portTickType ) 0 ) == pdFALSE )
					{
						/* The buffer could not be sent to the IP task so the
						buffer must be released. */
						vNetworkBufferRelease( pxNetworkBuffer );
						iptraceETHERNET_RX_EVENT_LOST();
					}
					else
					{
						iptraceNETWORK_INTERFACE_RECEIVE();
					}
				}
				else
				{
					/* The buffer does not contain any data so there is no
					point sending it to the IP task.  Just release it. */
					vNetworkBufferRelease( pxNetworkBuffer );
					iptraceETHERNET_RX_EVENT_LOST();
				}
			}
			else
			{
				iptraceETHERNET_RX_EVENT_LOST();
			}

			/* Release the descriptor. */
			EMAC_UpdateRxConsumeIndex();
		}
	}
}
/*-----------------------------------------------------------*/

