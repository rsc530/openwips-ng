/*
 * OpenWIPS-ng server.
 * Copyright (C) 2011 Thomas d'Otreppe de Bouvette
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *      Author: Thomas d'Otreppe de Bouvette
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <zlib.h>
#include "packet_analysis.h"
#include "common/defines.h"
#include "common/utils.h"
#include "plugins.h"

void init_packet_analysis()
{
	_packet_analysis_thread = PTHREAD_NULL;
	_message_list = NULL;
	_nb_messages = 0;
}

void free_global_memory_packet_analysis()
{
	int i;
	if (_message_list != NULL) {
		for (i = 0; i < _nb_messages; i++) {
			free(_message_list+i);
		}
		FREE_AND_NULLIFY(_message_list);
	}
}

int start_packet_analysis_thread()
{
	int thread_created = pthread_create(&_packet_analysis_thread, NULL, (void*)&packet_analysis_thread, NULL);
	if (thread_created != 0) {
		fprintf(stderr,"ERROR, failed to create packet analysis thread.\n");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

int kill_packet_analysis_thread()
{
	if (_packet_analysis_thread != PTHREAD_NULL) {

		_stop_packet_analysis_thread = 1;

		_packet_analysis_thread = PTHREAD_NULL;
	}

	return EXIT_SUCCESS;
}

int is_one_of_our_mac(unsigned char * mac)
{
	int i;

	if (mac != NULL && _nb_macs >= 0) {
		for (i = 0; i < _nb_macs; i++) {
			if (memcmp(_our_macs[i], mac, 6) == 0) {
				return 1;
			}
		}
	}

	return 0;
}

int packet_analysis_thread(void * data)
{
	int is_our_mac, plugin_potential_attack_in_progress;
	char * attack_details, is_attacked, do_attacked_check, plugin_check;
	struct pcap_packet * cur;
	struct packet_list * local_packet_list;
	struct plugin_info * cur_plugin;
	struct frame_plugin_functions * cur_frame_plugin_fct;
	struct timeval * timediff;
	uint32_t fcs;

	_packet_analysis_thread_stopped = 0;
	local_packet_list = init_new_packet_list();
	plugin_check = 0;

#ifdef DEBUG
	fprintf(stderr, "[*] Packet analysis thread started.\n");
#endif

	while (!_stop_threads && !_stop_packet_analysis_thread) {

		// Pointer to last packet analyzed: last_packet_analyzed
		if (_receive_packet_list->nb_packet <= 0) {
			usleep(500);
			continue;
		}

		// Get all packets
		add_multiple_packets_to_list(get_packets(INT_MAX, &_receive_packet_list), &local_packet_list, 0);

		if (local_packet_list->nb_packet == 0) {
			usleep(500);
			continue;
		}

		for (cur = local_packet_list->packets; cur != NULL; cur = cur->next) {
			// TODO: Think how to handle fragmentated packet (I mean reassembly)

			// TODO: Add a message telling we got invalid packet
			if (cur->header.cap_len < MIN_PACKET_SIZE + FCS_SIZE) {
				fprintf(stderr, "Received invalid packet - frame too short to be analyzed. Expected %d bytes, received %u.\n", MIN_PACKET_SIZE + FCS_SIZE, cur->header.cap_len);
				continue;
			}

			// Get packet information
			cur->info = parse_packet_basic_info(cur);

			if (cur->info == NULL) {
				continue;
			}

			// Check FCS before processing frame (ignore if invalid but log it in DB).
			if (cur->info->fcs_present) {
				fcs = crc32(0L, cur->info->frame_start, cur->header.cap_len - FCS_SIZE - cur->info->packet_header_len);
				if (fcs != cur->info->fcs) {
#ifdef EXTRA_DEBUG
					fprintf(stderr, "Invalid FCS: Got 0x%x, expected 0x%x. Ignoring frame.\n", cur->info->fcs, fcs);
#endif
					continue;
				}
			}

			// This is where the data will be analyzed and passed to plugins
			// Do basic analysis of the values
			if (cur->info->protocol > 0) { // TODO: Move that check into a plugin
				fprintf(stderr, "ANOMALY - Invalid protocol version <%u> for frame (SN: %u): it should always be 0.\n", cur->info->protocol, cur->info->sequence_number);
				// Don't process that frame

				continue;
			}

			// Apply all plugins
#ifdef DEBUG
			if (!plugin_check) {
				if (_plugin_frame) {
					printf("Checking frames against plugins.\n");
				} else {
					printf("No plugins installed.\n");
				}
				plugin_check = 1;
			}
#endif

			if (_plugin_frame) {
				// Check if mac address is one of ours
				is_our_mac = -1;

				for (cur_plugin = _plugin_frame; cur_plugin != NULL; cur_plugin = cur_plugin->next) {
					cur_frame_plugin_fct = (struct frame_plugin_functions *)(cur_plugin->plugin_specific_fct);

					// Ignore plugin if looking for a specific type of frame and it's not the same
					if (cur_frame_plugin_fct->settings.static_frame_type != cur->info->frame_type
							&& cur_frame_plugin_fct->settings.static_frame_type != -1) {
						continue;
					}

					// Ignore plugin if looking for a specific subtype of frame and it's not the same
					if (cur_frame_plugin_fct->settings.static_frame_subtype != cur->info->frame_subtype
											&& cur_frame_plugin_fct->settings.static_frame_subtype != -1) {
						continue;
					}

					if (is_our_mac == -1) {
						is_our_mac = is_one_of_our_mac(cur->info->address1) ||
										is_one_of_our_mac(cur->info->address2) ||
										is_one_of_our_mac(cur->info->address3) ||
										is_one_of_our_mac(cur->info->address4);
					}

					// Ignore plugin if it doesn't need all frame and it's not our macs
					if (!cur_frame_plugin_fct->settings.need_all_frames && !is_our_mac) {
						continue;
					}

					if (cur_frame_plugin_fct->settings.is_single_frame_attack) {
						do_attacked_check = cur_frame_plugin_fct->can_use_frame(cur, cur_plugin->plugin_data);

						if (do_attacked_check) {

							// Add that frame to the list
							add_packet_to_list(copy_packets(cur, 0,
									cur_frame_plugin_fct->settings.require_packet_parsed),
									&(cur_frame_plugin_fct->frame_list));
						}
					}
					else if (cur_frame_plugin_fct->can_use_frame(cur, cur_plugin->plugin_data)) {
						// Check if it is an attack

						plugin_potential_attack_in_progress = cur_frame_plugin_fct->analyze(cur, cur_plugin->plugin_data);
						if (cur_frame_plugin_fct->potential_attack_in_progress && !plugin_potential_attack_in_progress) {
							// Attack is done, reset values
							cur_frame_plugin_fct->potential_attack_in_progress = 0;
							cur_frame_plugin_fct->nb_frames_before_analysis = -1;
							cur_frame_plugin_fct->time_before_analysis = -1;

							// Clear plugin packet memory
							// TODO: Add a field that ask if packet list memory should be cleared when attack is finished.
							free_pcap_packet(&(cur_frame_plugin_fct->frame_list->packets), 0);
							cur_frame_plugin_fct->frame_list->nb_packet = 0;

						} else if (!(cur_frame_plugin_fct->potential_attack_in_progress) && plugin_potential_attack_in_progress) {
							cur_frame_plugin_fct->potential_attack_in_progress = 1;

							// Check the parameters of the attacks
							cur_frame_plugin_fct->nb_frames_before_analysis = cur_frame_plugin_fct->nb_frames_before_analyzing(cur_plugin->plugin_data);
							cur_frame_plugin_fct->time_before_analysis = cur_frame_plugin_fct->time_ms_before_analyzing(cur_plugin->plugin_data);
						}

						if (cur_frame_plugin_fct->potential_attack_in_progress) {
							// TODO: Disable mutex
							add_packet_to_list(copy_packets(cur, 0,
									cur_frame_plugin_fct->settings.require_packet_parsed),
									&(cur_frame_plugin_fct->frame_list));

							// Cleanup buffer
							if (cur_frame_plugin_fct->time_before_analysis > 0) { // Cleanup frame buffer for frame older than X ms
								remove_packet_older_than(cur, cur_frame_plugin_fct->time_before_analysis, &(cur_frame_plugin_fct->frame_list), 0);
							} else if (cur_frame_plugin_fct->nb_frames_before_analysis > 0) {
								// Keep the last X frames
								remove_first_X_packets(cur_frame_plugin_fct->frame_list->nb_packet - cur_frame_plugin_fct->nb_frames_before_analysis, &cur_frame_plugin_fct->frame_list, 0);
							}

							// Check if we have to check for an attack
							do_attacked_check = 0;
							if (cur_frame_plugin_fct->nb_frames_before_analysis <= 0 && cur_frame_plugin_fct->time_before_analysis <= 0) {
								// Always analyze if both values are -1
								do_attacked_check = 1;
							} else if (cur_frame_plugin_fct->nb_frames_before_analysis > 0 && cur_frame_plugin_fct->time_before_analysis > 0) {
								// Frame rate based attack
								// TODO: Might need to change this condition (order)
								if (cur_frame_plugin_fct->nb_frames_before_analysis <= cur_frame_plugin_fct->frame_list->nb_packet) {
									timediff = get_time_difference_between_packet(cur_frame_plugin_fct->frame_list->packets, cur);
									do_attacked_check = ((timediff->tv_sec * 1000) + (timediff->tv_usec / 1000) <= cur_frame_plugin_fct->time_before_analysis);
									free(timediff);
								}
							} else {
								if (cur_frame_plugin_fct->nb_frames_before_analysis > 0) { // Frame amount based attack
									// Check if frame amount meets our criteria
									do_attacked_check = (cur_frame_plugin_fct->nb_frames_before_analysis <= cur_frame_plugin_fct->frame_list->nb_packet);
								} else { // Time based attack
									timediff = get_time_difference_between_packet(cur_frame_plugin_fct->frame_list->packets, cur);
									do_attacked_check = ((timediff->tv_sec * 1000) + (timediff->tv_usec / 1000) >= cur_frame_plugin_fct->time_before_analysis);
									free(timediff);
								}
							}
						}
					}

					// Log attack
					if (do_attacked_check) {
						is_attacked = cur_frame_plugin_fct->is_attacked(cur_frame_plugin_fct->frame_list->packets, cur_plugin->plugin_data);
						if (is_attacked) {
							attack_details = cur_frame_plugin_fct->attack_details(cur_plugin->plugin_data);
							if (!STRING_IS_NULL_OR_EMPTY(attack_details)) {
								fprintf(stderr, "%s - %s\n", cur_plugin->name, attack_details);
							} else {
								fprintf(stderr, "%s - Currently attacked. Plugin did not provide details.\n", cur_plugin->name);
							}

							FREE_AND_NULLIFY(attack_details);
						}

						// Clear stuff and free memory
						if (is_attacked || cur_frame_plugin_fct->settings.is_single_frame_attack) {
							cur_frame_plugin_fct->clear_attack(cur_plugin->plugin_data);
							free_pcap_packet(&(cur_frame_plugin_fct->frame_list->packets), is_attacked);
							cur_frame_plugin_fct->frame_list->packets = NULL;
							cur_frame_plugin_fct->frame_list->nb_packet = 0;
							is_attacked = 0;
						}
					}
				}
			}
		}

		// Cleanup list
		free_pcap_packet(&(local_packet_list->packets), 1);
		local_packet_list->packets = NULL;
		local_packet_list->nb_packet = 0;

		// Make sure the CPU won't get overloaded
		usleep(10);
	}

	_packet_analysis_thread_stopped = 1;

	return EXIT_SUCCESS;
}
