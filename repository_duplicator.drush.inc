<?php

/**
 * @file
 * Repository duplicator drush command.
 */

/**
 * Implements hook_drush_command().
 */
function repository_duplicator_drush_command() {
  return array(
    'duplicate-repository' => array(
      'description' => dt('Takes objects from one namespace and duplicates them in a different namespace.'),
      'drupal_dependencies' => array('islandora'),
      'arguments' => array(
        'source_namespace' => dt('The namespace to copy objects from'),
        'destination_namespace' => dt('The namespace to use for the new copies.'),
      ),
      'required_arguments' => TRUE,
    ),
  );
}

/**
 * Repository duplication drush command.
 *
 * @param string $source_namespace
 *   The namespace to find objects to duplicate from.
 * @param string destination_namespace
 *   The namespace to create copies in.
 */
function drush_repository_duplicator_duplicate_repository($source_namespace, $destination_namespace) {
  // Set the batch, and then start it.
  batch_set(repository_duplicator_duplicate_repository_batch($source_namespace, $destination_namespace));
  drush_backend_batch_process();
}

/**
 * Batch process definition for the repository duplicator command.
 *
 * @param string $source_namespace
 *   The namespace to find objects to duplicate from.
 * @param string destination_namespace
 *   The namespace to create copies in.
 *
 * @return array
 *   The batch array definition.
 */
function repository_duplicator_duplicate_repository_batch($source_namespace, $destination_namespace) {
  return array(
    'title' => t('Repository Duplicator Duplicate Repository'),
    'init_message' => t("Preparing to copy objects from the $source_namespace namespace to the $destination_namespace namespace."),
    'progress_message' => t('Time elapsed: @elapsed <br/>Estimated time remaining: @estimate.'),
    'error_message' => t('An error has occured.'),
    'operations' => array(
      array(
        'repository_duplicator_duplicate_repository_batch_operation',
        array($source_namespace, $destination_namespace),
      ),
    ),
  );
}

/**
 * Batch operation for repository duplicator.
 *
 * @param string $source_namespace
 *   The namespace to find objects to duplicate from.
 * @param string destination_namespace
 *   The namespace to create copies in.
 * @param array $context
 *   The batch context array.
 */
function repository_duplicator_duplicate_repository_batch_operation($source_namespace, $destination_namespace &$context) {
  $sandbox = &$context['sandbox'];
  // Establish the connection to tuque; this is where the magic starts.
  $connection = islandora_get_tuque_connection();
  // If this is our first pass through the batch operation, the batch context's
  // sandbox will be empty, so we know that we have variables to initialize.
  if (empty($sandbox)) {
    // Specifically, we need to know where we are in the batch (current_index),
    // how many things are in the batch (total), and whether or not we are
    // done of the batch (finished, also, maybe?).
    $sandbox['current_index'] = 0;
    $sandbox['total'] = repository_duplicator_get_object_count_from_namespace($connection->repository, $source_namespace);
    $context['finished'] = $sandbox['total'] === 0 ? 1 : 0;
  }
  // Grab the next object in the namespace.
  $objects = repository_duplicator_get_object_set_from_namespace($connection->repository, $source_namespace, $sandbox['current_index']);
  // Duplicate the object represented by the return value from the query.
  $context['message'] = repository_duplicator_duplicate_object($connection->repository, $object[0]['pid']['value'], $destination_namespace);
  // Finish if this was the last object.
  $context['finished'] = $sandbox['current_index'] / $sandbox['total'];
  // Increment the query offset, otherwise.
  $sandbox['current_index']++;
}

/**
 * Returns the appropriate SPARQL query for the source namespace and offset.
 *
 * @param string $source_namespace
 *   The namespace to search for objects in.
 * @param int $offset
 *   The amount to offset the query by. Defaults to 0.
 *
 * @return string
 *   A SPARQL query for finding all objects based on a given namespace at a
 *   given offset, minus content models.
 */
function repository_duplicator_get_namespace_query($source_namespace, $offset = 0) {
  // To start, we want to SELECT ?pids from the resource index.
  $query = <<<EOQ
SELECT ?pid
FROM <#ri>
EOQ;

  // Next, we're narrowing our search to ?pids that have the FedoraObject
  // content model.
  $query .= <<<EOQ
WHERE {
  ?pid <fedora-model:hasModel> <info:fedora/fedora-system:FedoraObject-3.0>
EOQ;

  // Now, it gets a bit tricky. To remove ContentModel objects, we'll need to
  // set up an OPTIONAL matching pattern for the ContentModel so that we can
  // later filter on it if it isn't BOUND to a value on the graph. Negation
  // as failure ftw.
  // XXX: We shouldn't have to add createdDate to our OPTIONAL pattern - it
  // should be as simple as filtering any ?pid hasModel ?model inside the
  // OPTIONAL where ?model is ContentModel. However, Mulgara doesn't match the
  // spec properly, and requires an object to be found for the OPTIONAL to work.
  // So, we match both a property that might be there (ContentModel), and a
  // property that will definitely be there (createdDate), and filter on the
  // property that will definitely be there in cases where they're both set.
  $query .= <<<EOQ
    OPTIONAL {
      ?pid <fedora-model:hasModel> <info:fedora/fedora-system:ContentModel-3.0> ;
           <fedora-model:createdDate> ?date .
    }
  FILTER(!BOUND(?date))
EOQ;

  // Next, we filter all results from the graph where the string literal version
  // of the ?pid starts with info:fedora/namespace:. The carat reads from the
  // start of a string in regex.
  $query .= <<<EOQ
  FILTER(REGEX(STR(?pid), "^info:fedora/$source_namespace:"))
}
EOQ;

  // Lastly, we establish the offset to start the result set from.
  $query .= <<<EOQ
OFFSET $offset
EOQ;

  // Return the generated query.
  return $query;
}

/**
 * Gets the count of existing objects in the RI for the given namespace.
 *
 * @param IslandoraFedoraRepository $repository
 *   The repository to query in.
 * @param $source_namespace
 *   The namespace to filter the query by.
 *
 * @return int
 *   The number of objects in the RI for the given namespace.
 */
function repository_duplicator_get_object_count_from_namespace($repository, $source_namespace) {
  $query = repository_duplicator_get_namespace_query($source_namespace);
  return $repository->ri->countQuery($query, 'sparql');
}

/**
 * Gets an object from the RI for the given namespace at the given offset.
 *
 * @param IslandoraFedoraRepository $repository
 *   The repository to query in.
 * @param string $source_namespace
 *   The namespace to filter the query by.
 * @param int $offset
 *   The amount to offset the query by.
 *
 * @return array
 *   An array representing the result of the query.
 */
function repository_duplicator_get_object_set_from_namespace($repository, $source_namespace, $offset) {
  $query = repository_duplicator_get_namespace_query($source_namespace, $offset);
  return $repository->ri->sparqlQuery($query, 1);
}

/**
 * Generates a new object using an existing object properties.
 *
 * @param IslandoraFedoraRepository $repository
 *   The repository to use when manipulating objects.
 * @param string $pid
 *   The PID of the source object.
 * @param string $namespace
 *   The namespace to use for the copied object.
 *
 * @return string
 *   A message giving the details of this particular copy operation.
 */
function repository_duplicator_duplicate_object($repository, $pid, $namespace) {
  // Grab the original object, so that we can retrieve its properties,
  // relationships and datastreams.
  $former_object = islandora_object_load($pid);
  // Construct a new object in the repository. This object does not yet exist
  // inside Fedora; instead it only objectively represents the idea of an object
  // that we can manipulate before ingesting properly.
  $new_object = $repository->constructObject($namespace);

  // Set simple properties. The array_walk() method will let us iterate through
  // the properties we define below, setting them to the new object.
  $properties_to_set = array(
    'label' => $former_object->label,
    'owner' => $former_object->owner,
    'state' => $former_object->state,
  );
  array_walk($properties_to_set, 'repository_duplicator_copy_property', &$new_object);

  // Set the relationships.
  foreach ($former_object->relationships->get() as $relationship) {
    // Check the relationships->get() documentation for more details. Existing
    // relationships are returned as an array containing predicate and object
    // info, segmented ito parts including values.
    $new_object->relationships->add($relationship['predicate']['namespace'], $relationship['predicate']['value'], $relationship['object']['value']);
  }
  // Ingest the object before we start adding datastreams.
  $repository->ingestObject($new_object);

  // Iterate through the datastreams.
  // XXX: In most cases, iterating through datastreams via the object itself is
  // preferable (e.g., foreach ($former_object as $loaded_datastream)); however,
  // we MUST copy RELS-INT last or Fedora might yell at us for trying to write
  // RELS-INT statements referencing datastreams that don't exist.
  $datastreams = array_keys($repository->api->a->listDatastreams($pid));
  if (($dsid = array_search('RELS-INT', $datastreams)) !== FALSE) {
    unset($datastreams[$dsid]);
    $datastreams[] = 'RELS-INT';
  }
  // We now have just a list of unloaded datastreams, so we'll have to load
  // each one we iterate through.
  foreach ($datastreams as $datastream) {
    $former_datastream = islandora_datastream_load($datastream, $former_object);
    // Next, we construct a new datastream. This datastream is not yet ingested,
    // which will happen after it is set up.
    $new_datastream = $new_object->constructDatastream($former_datastream->id, $former_datastream->controlGroup);
    // This works the same as the array_walk for the object properties.
    $properties_to_set = array(
      'label' => $former_datastream->label,
      'mimetype' => $former_datastream->mimetype,
      'state' => $former_datastream->state,
      'versionable' => $former_datastream->versionable,
    );
    // Inline (X)ML and (M)anaged datastreams store datastream 'content', while
    // (R)edirect and (E)xternal-referenced datastreams store a datastream
    // 'url', so we need to make that distinction here.
    if ($former_datastream->controlGroup === 'X' || 'M') {
      // References to the old PID should be replaced with the new PID.
      $properties_to_set['content'] = str_replace($pid, $new_object->id, $former_datastream->content);
    }
    else {
      $properties_to_set['url'] = $former_datastream->url;
    }
    array_walk($properties_to_set, 'repository_duplicator_copy_property', &$new_datastream);
    // Finally, ingest our fully-realized datastream.
    $new_object->ingestDatastream($new_datastream);
  }

  // Return a message.
  return "Copied object $pid to $new_object->id.";
}

/**
 * Copies a property from an old object or datastream to a new one.
 *
 * Used by array_walk() to take an associative array of property names and
 * former values and apply it to a new object or datastream.
 *
 * @param string $former
 *   The former value of the property.
 * @param string $property
 *   The name of the property to apply on the new object.
 * @param object $new
 *   The new object to apply the property to.
 */
function repository_duplicator_copy_property($former, $property, &$new) {
  $new->{$property} = $former;
}
