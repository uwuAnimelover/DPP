/************************************************************************************
 *
 * D++, A Lightweight C++ library for Discord
 *
 * Copyright 2021 Craig Edwards and D++ contributors 
 * (https://github.com/brainboxdotcc/DPP/graphs/contributors)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/
#pragma once
#include <dpp/export.h>
#include <dpp/discord.h>
#include <dpp/json_fwd.hpp>

namespace dpp {

#define MAX_EMOJI_SIZE 256 * 1024

/**
 * @brief Flags for dpp::emoji
 */
enum emoji_flags {
	/// Emoji requires colons
	e_require_colons = 0b00000001,
	/// Managed (introduced by application)
	e_managed =        0b00000010,
	/// Animated
	e_animated =       0b00000100,
	/// Available (false if the guild doesn't meet boosting criteria, etc)
	e_available =      0b00001000,
};

/**
 * @brief Represents an emoji for a dpp::guild
 */
class CoreExport emoji : public managed {
public:
	/**
	 * @brief Emoji name
	 */
	std::string name;
	/**
	 * @brief User id who uploaded the emoji
	 */
	snowflake user_id;
	/**
	 * @brief Flags for the emoji from dpp::emoji_flags
	 */
	uint8_t flags;
	/**
	 * @brief Image data for the emoji if uploading
	 */
	std::string* image_data;
	
	/**
	 * @brief Construct a new emoji object
	 */
	emoji();

	/**
	 * @brief Construct a new emoji object with name, ID and flags
	 * 
	 * @param name The emoji's name
	 * @param id ID, if it has one (unicode does not)
	 * @param flags Emoji flags (emoji_flags)
	 */
	emoji(const std::string, const snowflake = 0, const uint8_t = 0);

	/**
	 * @brief Destroy the emoji object
	 */
	virtual ~emoji();

	/**
	 * @brief Read class values from json object
	 * 
	 * @param j A json object to read from
	 * @return A reference to self
	 */
	emoji& fill_from_json(nlohmann::json* j);

	/**
	 * @brief Build the json for this object
	 * 
	 * @param with_id include the id in the JSON
	 * @return std::string json data
	 */
	std::string build_json(bool with_id = false) const;

	/**
	 * @brief Emoji requires colons
	 * 
	 * @return true Requires colons
	 * @return false Does not require colons
	 */
	bool requires_colons() const;

	/**
	 * @brief Emoji is managed
	 * 
	 * @return true Is managed
	 * @return false Is not managed
	 */
	bool is_managed() const;

	/**
	 * @brief Emoji is animated
	 * 
	 * @return true Is animated
	 * @return false Is noy animated
	 */
	bool is_animated() const;

	/**
	 * @brief Is available
	 * 
	 * @return true Is available
	 * @return false Is unavailable
	 */
	bool is_available() const;

	/**
	 * @brief Load an image into the object as base64
	 * 
	 * @param image_blob Image binary data
	 * @param type Type of image
	 * @return emoji& Reference to self
	 */
	emoji& load_image(const std::string &image_blob, image_type type);

	/**
	 * @brief Format to name if unicode, name:id if has id or a:name:id if animated
	 * 
	 * @return Formatted name for reactions
	 */
	std::string format() const;
};

/**
 * @brief Group of emojis
 */
typedef std::unordered_map<snowflake, emoji> emoji_map;

};
