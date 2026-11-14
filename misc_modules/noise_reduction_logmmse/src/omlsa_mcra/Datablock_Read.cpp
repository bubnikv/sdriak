#include <math.h>
#include "Datablock_Read.h"
using namespace std;

Datablock_Read::Datablock_Read(int sample_rate,int MaxDataLen)
{
	m_sample_rate = sample_rate;
	m_derr_code=Initial(MaxDataLen);
}

short Datablock_Read::Initial(int MaxDataLen) {
	m_maxdata = MaxDataLen;
	if (m_sample_rate < 23000)	// sample rate below 23 kHz
	{
		m_wlen = 256;
		m_inc = 128;  // frame length is a power of 4
	}
	else {
		m_wlen = 1024;
		m_inc = 512;
	}
	m_wlen15 = m_wlen +(m_wlen>>1);

	m_inc_move = log(m_inc) / log(2);

	m_DoubDataBuffer = new short[MaxDataLen + m_wlen];
	if (!m_DoubDataBuffer) {
		return -52;
	}
	m_data_in = new short[m_wlen15];
	if (!m_data_in) {
		return -53;
	}
	m_data_storage = new short[m_wlen15];
	if (!m_data_storage) {
		return -54;
	}
	m_process_storage=new int[m_wlen15];
	if (!m_process_storage) {
		return -55;
	}
	m_data_out = new short[m_wlen15];
	if (!m_data_out) {
		return -57;
	}
	m_data_resize = new short[MaxDataLen + m_wlen];
	if (!m_data_resize) {
		return -58;
	}

	memset(m_process_storage, 0, m_wlen15 * sizeof(int));
	memset(m_DoubDataBuffer, 0, (MaxDataLen + m_wlen) * sizeof(short));
	m_data_rest_length = 0;
	m_blockInd = 0;
	short m_ierr_code = LSA.Initialize(m_wlen15); //15--49
	if (m_ierr_code < 0)return m_ierr_code;
	return 0;
}

// IN-PLACE: safe (pInBuffer == pOutBuffer).
short Datablock_Read::Data_procese(short* pInBuffer, short* pOutBuffer,int read_length,int& Out_Length){
	if (m_derr_code < 0) return m_derr_code;
	if (pInBuffer == NULL)return -50;
	if (pOutBuffer == NULL)return -51;

	int current_total_data = read_length + m_data_rest_length;  // total samples after prepending the carry-over

	if (current_total_data < m_wlen15) {	// less than one full frame of data
		memcpy(m_data_storage + m_data_rest_length, pInBuffer, (read_length) * sizeof(short));

		Out_Length = 0;
		m_data_rest_length = current_total_data;
	}
	else {
	// merge the data, accounting for the stored first-frame carry-over
	// first copy in the carried-over samples
	memcpy(m_data_resize, m_data_storage, (m_data_rest_length) * sizeof(short));
	// then append the newly read samples
	memcpy(m_data_resize + m_data_rest_length, pInBuffer, read_length * sizeof(short));

 	int data_use = 0;				 // samples already consumed, counted at frame starts
	int current_frame = ((current_total_data - m_wlen) >> m_inc_move) + 1;  // number of frames the current data can form
	// round the frame count down to even; the odd remainder is carried over
	current_frame = (current_frame | 1) - 1;

	m_data_rest_length = current_total_data -( current_frame*m_inc);  // update the carry-over: samples past the last frame
	Out_Length = current_frame * m_inc;	  // output length of this segment

	for (int k = 0; k < current_frame-1; k+=2, m_blockInd+=2){
		memcpy(m_data_in, m_data_resize + data_use, m_wlen15 * sizeof(short)); // split into frames
		memset(m_data_out, 0, sizeof(short)*m_wlen15);
		m_derr_code=LSA.Denoise_process(m_data_in, m_data_out, m_blockInd);

		if (m_derr_code < 0) return m_derr_code;
		// output: the two overlap-added frames
 		int block_inc = k * m_inc;  // current position within the segment
		if (current_frame == 2) {   
			for (int i = 0; i < m_inc; i++) {
				m_DoubDataBuffer[block_inc + i] += m_data_out[i];
				m_DoubDataBuffer[block_inc + i] += m_process_storage[i];
				m_DoubDataBuffer[block_inc + m_inc+i] += m_data_out[i+m_inc];
				m_process_storage[i] = m_data_out[i+m_wlen];
			}
		}
		else {
			if (k == 0) {                    
				for (int i = 0; i < m_inc; i++) {
					m_DoubDataBuffer[block_inc + i] += m_data_out[i];
					m_DoubDataBuffer[block_inc + i] += m_process_storage[i];
					m_DoubDataBuffer[block_inc + m_inc + i] += m_data_out[i + m_inc];
					m_DoubDataBuffer[block_inc + m_wlen + i ] += m_data_out[i + m_wlen];
				}
			}
			else if (k == current_frame-2 ){
				for (int i = 0; i < m_inc; i++) {   
					m_DoubDataBuffer[block_inc + i] += m_data_out[i];
					m_DoubDataBuffer[block_inc + m_inc + i] += m_data_out[i + m_inc];
					m_process_storage[i] = m_data_out[i + m_wlen];
				}
			}
			else {   // middle frames of the segment
				for (int i = 0; i < m_wlen15; i++) {
					m_DoubDataBuffer[block_inc + i] += m_data_out[i];
				}
			}
		}
		for (int i = 0; i < Out_Length; i++) {
			pOutBuffer[i] = m_DoubDataBuffer[i];
		}
		data_use += m_wlen;
	}
	// store the residual samples
	memcpy(m_data_storage, m_data_resize + data_use, (m_data_rest_length) * sizeof(short));
	memset(m_DoubDataBuffer, 0, (m_maxdata + m_wlen) * sizeof(short));
	}
	return 0;
}

	 
Datablock_Read::~Datablock_Read()
{
	if (!m_data_storage) {
		delete[] m_data_storage;
		m_data_storage = NULL;
	}
	if (!m_data_in) {
		delete[] m_data_in;
		m_data_in = NULL;
	}
	if (!m_data_resize) {
		delete[] m_data_resize;
		m_data_resize = NULL;
	}
	if (!m_DoubDataBuffer) {
		delete[] m_DoubDataBuffer;
		m_DoubDataBuffer = NULL;
	}
	if (!m_data_out) {
		delete[] m_data_out;
		m_data_out = NULL;
	}
}

