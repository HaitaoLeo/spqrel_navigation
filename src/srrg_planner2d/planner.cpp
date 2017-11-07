#include "planner.h"

namespace srrg_planner {
  Planner::Planner() {
    _max_cost = 100.0;
    _min_cost = 20.0;
    _robot_radius = 0.3;
    _safety_region = 1.0;

    _use_gui = false;
    _what_to_show = Map;

    _have_goal = false;
    _have_goal_with_angle = false;
    _goal = Eigen::Vector3f(0,0,0);
    _goal_image = Eigen::Vector3f(0,0,0);
    _goal_pixel = Eigen::Vector2i(0,0);

    _robot_pose = Eigen::Vector3f(0,0,0);
    _robot_pose_image = Eigen::Vector3f(0,0,0);
    _robot_pose_pixel = Eigen::Vector2i(0,0);

    _laser_points.clear();
    _dyn_map.clearPoints();

    //_state = WaitingForMap;
  }

  void Planner::cancelGoal() {
    _have_goal = false;
    _path.clear();
    _velocities = Eigen::Vector2f::Zero();
    _motion_controller.resetVelocities();
    stopRobot();
  }
  
  void Planner::reset(){
    cancelGoal();

    //Restarting Path Map variables and removing obstacles
    restartPathMap();    
  }

  void Planner::restartPathMap(){
    _dyn_map.clearPoints();
    
    int occ_threshold = (1.0 - _occ_threshold) * 255;
    int free_threshold = (1.0 - _free_threshold) * 255;
    grayMap2indices(_indices_image, _map_image, occ_threshold, free_threshold);


    _dmap_calculator.setMaxDistance(_safety_region/_map_resolution);
    _dmap_calculator.setIndicesImage(_indices_image);
    _dmap_calculator.setOutputPathMap(_distance_map);
    _dmap_calculator.init();
    _max_distance_map_index = _dmap_calculator.maxIndex();
    _dmap_calculator.compute();
    _distance_map_backup=_distance_map.data();
    
    //I'm doing also backup of the cost_map without obstacles
    _distance_image = _dmap_calculator.distanceImage()*_map_resolution;
    distances2cost(_cost_image_backup,
		   _distance_image, _robot_radius, _safety_region, _min_cost, _max_cost);
    distances2cost(_cost_image,
		   _distance_image, _robot_radius, _safety_region, _min_cost, _max_cost);
  }
  
  void Planner::readMap(const std::string mapname){
    std::cerr << "Reading map" << mapname << std::endl;
  
    // reading map info
    SimpleYAMLParser parser;
    parser.load(mapname);
    std::cerr << "Dirname: " << dirname(strdup(mapname.c_str())) << std::endl;
    
    std::string map_image_name = parser.getValue("image");
    float map_resolution = parser.getValueAsFloat("resolution");
    float occ_threshold = parser.getValueAsFloat("occupied_thresh");
    float free_threshold = parser.getValueAsFloat("free_thresh");
    Eigen::Vector3f map_origin = parser.getValueAsVector3f("origin");
    
    std::string full_path_map_image = std::string(dirname(strdup(mapname.c_str())))+"/"+map_image_name;
    std::cerr << "Opening image" << full_path_map_image << std::endl;
  
    UnsignedCharImage map_image = cv::imread(full_path_map_image, CV_LOAD_IMAGE_GRAYSCALE);

    setMapFromImage(map_image, map_resolution, map_origin, occ_threshold, free_threshold);
  }



  void Planner::setMapFromImage(const UnsignedCharImage& map_image, const float map_resolution,
				const Eigen::Vector3f& map_origin, const float occ_threshold, const float free_threshold) {

    _mtx_display.lock();

    _map_image = map_image.clone();
    _map_resolution = map_resolution;
    _map_inverse_resolution = 1./_map_resolution;
    _map_origin = map_origin;
    _map_origin_transform_inverse = v2t(_map_origin).inverse();
    _occ_threshold = occ_threshold;
    _free_threshold = free_threshold;

    // _map_origin: reference system bottom-left, X right, Y up  (values read from yaml file) (ROS convention)
    // _image_map_origin: image reference system top-left, X down, Y right (opencv convention)

    // transform from _map_origin to _image_map_origin
    Eigen::Vector3f map_to_image(0, _map_image.rows*_map_resolution, -M_PI/2);
    Eigen::Isometry2f tf = v2t(_map_origin) * v2t(map_to_image);
    _image_map_origin = t2v(tf);
    _image_map_origin_transform_inverse = v2t(_image_map_origin).inverse();;

    restartPathMap();

    std::cerr << "Setting map: \n"
	      << "  Size: " << map_image.rows << "x" << map_image.cols << std::endl
	      << "  Resolution: " << _map_resolution << std::endl
	      << "  Occ threshold: " << _occ_threshold << std::endl
	      << "  Free threshold: " << _free_threshold << std::endl
	      << "  Map origin: " << _map_origin.transpose() << std::endl;
      
    _mtx_display.unlock();

  }

  void Planner::initGUI(){
    _use_gui=true;
    cv::namedWindow( "spqrel_planner", 0 );
    cv::setMouseCallback( "spqrel_planner", &Planner::onMouse, this );
    handleGUIDisplay();
    std::cerr << std::endl;
    std::cerr << "GUI initialized" << std::endl;
    std::cerr << "Use the following keys on the GUI:" << std::endl;
    std::cerr << "M: change to map view" << std::endl;
    std::cerr << "D: change to distance map view" << std::endl;
    std::cerr << "C: change to cost map view" << std::endl;
    std::cerr << "R: reset planner state (clear dynamic obstacles and current goal)" << std::endl;
    std::cerr << std::endl;
  }

  void Planner::onMouse( int event, int x, int y, int flags, void* v){
    Planner* n=reinterpret_cast<Planner*>(v);
  
    if (event == cv::EVENT_LBUTTONDOWN && ((flags & cv::EVENT_FLAG_CTRLKEY) != 0) ) {
      std::cerr << "Left Click!" << std::endl;
      n->setGoalGUI(Eigen::Vector2i(y,x));
    }
  }

  void Planner::setGoalGUI(Eigen::Vector2i goal){
    _mtx_display.lock();
    //Goal given in pixels
    _goal_pixel = goal;
    //Transform from pixels to image [m]
    Eigen::Vector2f goal_image_xy = grid2world(goal);
    _goal_image = Eigen::Vector3f(goal_image_xy.x(), goal_image_xy.y(), 0);
    //Transform from image to map_origin
    Eigen::Isometry2f goal_transform = v2t(_image_map_origin) * v2t(_goal_image);
    _goal = t2v(goal_transform);
    _have_goal = true;
    _have_goal_with_angle = false;
    std::cerr << "Setting goal: " << _goal_pixel.transpose() << std::endl;
    _mtx_display.unlock();
  }

  void Planner::setGoal(const Eigen::Vector3f& goal){
    // Goal in map coordinates

    _mtx_display.lock();
    _have_goal = true;
    _have_goal_with_angle = true;
    //Goal given wrt map_origin
    _goal = goal;
    //From map_origin to image
    Eigen::Isometry2f goal_transform=_image_map_origin_transform_inverse*v2t(_goal);
    _goal_image = t2v(goal_transform); // image coordinates
    //From image to pixels
    _goal_pixel = world2grid(Eigen::Vector2f(_goal_image.x(), _goal_image.y()));  // pixel

    _mtx_display.unlock();
  }

  void Planner::updateGoals(){
    _mtx_display.lock();
     //From map_origin to image
    Eigen::Isometry2f goal_transform=_image_map_origin_transform_inverse*v2t(_goal);
    _goal_image = t2v(goal_transform); // image coordinates
    //From image to pixels
    _goal_pixel = world2grid(Eigen::Vector2f(_goal_image.x(), _goal_image.y()));  // pixel
    _mtx_display.unlock();
  }
  
  void Planner::setRobotPose(const Eigen::Vector3f& robot_pose){
    _mtx_display.lock();
  
    _robot_pose=robot_pose;
  
    Eigen::Isometry2f robot_pose_transform = _image_map_origin_transform_inverse*v2t(robot_pose);
    _robot_pose_image = t2v(robot_pose_transform);
    _robot_pose_pixel = world2grid(Eigen::Vector2f(_robot_pose_image.x(), _robot_pose_image.y()));
  
    _mtx_display.unlock();
  }

  void Planner::setLaserPoints(const Vector2fVector& laser_points){
    _mtx_display.lock();

    _laser_points = laser_points;

    _mtx_display.unlock();
  }

  void Planner::handleGUI(){
    handleGUIInput();
    handleGUIDisplay();
  }
  
  void Planner::handleGUIInput(){
    char key=cv::waitKey(25);
    switch(key) {
    case 'h':
      std::cout << "m: map mode" << std::endl;
      std::cout << "d: distance map" << std::endl;
      std::cout << "c: cost map" << std::endl;
      std::cout << "p: enable/disable motion" << std::endl;
      std::cout << "r: reset distance/cost map and remove the goal" << std::endl
		<< "   (can be used for emergency stop)" << std::endl;
      std::cout << "o: enable/disable external collision protection" << std::endl;
      break;
    case 'm':
      if (_what_to_show == Map)
	break;
      std::cerr << "Switching view to map" << std::endl;
      _what_to_show = Map;
      break;
    case 'd': 
      if (_what_to_show == Distance)
	break;
      std::cerr << "Switching view to distance map" << std::endl;
      _what_to_show = Distance;
      break;
    case 'c': 
      if (_what_to_show == Cost)
	break;
      std::cerr << "Switching view to cost map" << std::endl;
      _what_to_show = Cost;
      break;
      /*case 'p':    
      _move_enabled = !_move_enabled;
      std::cerr << "Move enabled: " << _move_enabled << std::endl;
      break;*/
    case 'r':
      std::cerr << "Resetting" << std::endl;
      reset();
      break;
      /*case 'o':
      _collision_protection_desired = ! _collision_protection_desired;
      std::cerr << "External Collision Protection Desired: " << _collision_protection_desired << std::endl;*/
    default:;
    }
  }

  void Planner::handleGUIDisplay() {
    _mtx_display.lock();

    FloatImage shown_image;
    switch(_what_to_show){
    case Map:
      shown_image.create(_indices_image.rows, _indices_image.cols);
      for (int r=0; r<_indices_image.rows; ++r) {
	int* src_ptr=_indices_image.ptr<int>(r);
	float* dest_ptr=shown_image.ptr<float>(r);
	for (int c=0; c<_indices_image.cols; ++c, ++src_ptr, ++dest_ptr){
	  if (*src_ptr<-1)
	    *dest_ptr = .5f;
	  else if (*src_ptr == -1)
	    *dest_ptr = 1.f;
	  else
	    *dest_ptr=0.f;
	}
      }
      break;
	
    case Distance:
      shown_image=_distance_image*(1./_safety_region);
      break;
    case Cost:
      shown_image=_cost_image*(1.f/_max_cost);
      break;
    }
      
    int robot_radius_pixel = _robot_radius*_map_inverse_resolution;
    // Drawing goal
    if (_have_goal){
      cv::circle(shown_image, cv::Point(_goal_pixel.y(), _goal_pixel.x()), robot_radius_pixel, cv::Scalar(0.0f));
      if (_have_goal_with_angle){
	Eigen::Vector2i goal_angle_pixel(cos(_goal_image.z())*robot_radius_pixel, sin(_goal_image.z())*robot_radius_pixel);
	cv::line(shown_image,
		 cv::Point(_goal_pixel.y(), _goal_pixel.x()),
		 cv::Point(_goal_pixel.y()+goal_angle_pixel.y(), _goal_pixel.x()+goal_angle_pixel.x()),
		 cv::Scalar(0.0f));
      }
    }

    // Drawing current pose
    cv::rectangle(shown_image,
		  cv::Point(_robot_pose_pixel.y()+robot_radius_pixel, _robot_pose_pixel.x()-robot_radius_pixel),
		  cv::Point(_robot_pose_pixel.y()-robot_radius_pixel, _robot_pose_pixel.x()+robot_radius_pixel),
		  cv::Scalar(0.0f));
    Eigen::Vector2i robot_pose_angle_pixel(cos(_robot_pose_image.z())*robot_radius_pixel, sin(_robot_pose_image.z())*robot_radius_pixel);
    cv::line(shown_image,
	     cv::Point(_robot_pose_pixel.y(), _robot_pose_pixel.x()),
	     cv::Point(_robot_pose_pixel.y()+robot_pose_angle_pixel.y(), _robot_pose_pixel.x()+robot_pose_angle_pixel.x()),
	     cv::Scalar(0.0f));
      
    //Draw path
    if (_have_goal && _path.size()>1){
      for (size_t i=0; i<_path.size()-1; i++){
	Eigen::Vector2i cell_from = _path[i];
	Eigen::Vector2i cell_to   = _path[i+1];
	cv::line(shown_image,
		 cv::Point(cell_from.y(), cell_from.x()),
		 cv::Point(cell_to.y(), cell_to.x()),
		 cv::Scalar(0.0f));
      }
      /*
	if (_nominal_path.size()>1){
	for (size_t i=0; i<_nominal_path.size()-1; i++){

	Eigen::Vector2i cell_from = _nominal_path[i];
	Eigen::Vector2i cell_to   = _nominal_path[i+1];
	cv::line(shown_image,
	cv::Point(cell_from.y(), cell_from.x()),
	cv::Point(cell_to.y(), cell_to.x()),
	cv::Scalar(0.5f));
	  
	  
	}
	}*/
    }

    //Draw laser
    for (size_t i=0; i<_laser_points.size(); i++){
      Eigen::Vector2f lp=v2t(_robot_pose_image)* _laser_points[i];
      int r = lp.x()*_map_inverse_resolution;
      int c = lp.y()*_map_inverse_resolution;
      if (! _distance_map.inside(r,c))
	continue;
      cv::circle(shown_image, cv::Point(c, r), 3, cv::Scalar(1.0f));
    }

    /*
      char buf[1024];
      sprintf(buf, " MoveEnabled: %d", _move_enabled);
      cv::putText(shown_image, buf, cv::Point(20, 50), cv::FONT_HERSHEY_SIMPLEX, shown_image.rows*1e-3, cv::Scalar(1.0f), 1);
      sprintf(buf, " CollisionProtectionDesired: %d", _collision_protection_desired);
      cv::putText(shown_image, buf, cv::Point(20, 50+(int)shown_image.cols*0.03), cv::FONT_HERSHEY_SIMPLEX, shown_image.rows*1e-3, cv::Scalar(1.0f), 1);
      sprintf(buf, " ExternalCollisionProtectionEnabled: %d", _collision_protection_enabled);
      cv::putText(shown_image, buf, cv::Point(20, 50+(int)shown_image.cols*0.06), cv::FONT_HERSHEY_SIMPLEX, shown_image.rows*1e-3, cv::Scalar(1.0f), 1);
    */
      
    cv::imshow("spqrel_planner", shown_image);

    _mtx_display.unlock();
    
    
  }
  
  void Planner::plannerStep(){
    if (!_have_goal)
      return;

    std::chrono::steady_clock::time_point time_start = std::chrono::steady_clock::now();

    //Get nominal path without obstacles
    //computePath(_cost_image_backup, _path_map_backup, _goal_pixel, _nominal_path);

    //Adding obstacles
    std::chrono::steady_clock::time_point time_dmap_start = std::chrono::steady_clock::now();
    _distance_map.data()=_distance_map_backup;

    if (_laser_points.size()>0) {
	  
      _dyn_map.setMapResolution(_map_resolution);
      _dyn_map.setRobotPose(_robot_pose_image);
      _dyn_map.setCurrentPoints(_laser_points);
      _dyn_map.compute();
      Vector2iVector obstacle_points;
      _dyn_map.getOccupiedCells(obstacle_points);
    
      _dmap_calculator.setPoints(obstacle_points, _max_distance_map_index);
      _dmap_calculator.compute();
    
    }else{
      std::cerr << "WARNING: laser data not available." << std::endl;
    }
  
    _distance_image = _dmap_calculator.distanceImage()*_map_resolution;
    distances2cost(_cost_image, _distance_image, _robot_radius, _safety_region, _min_cost, _max_cost);

    std::chrono::steady_clock::time_point time_dmap_end = std::chrono::steady_clock::now();
    std::cerr << "DMapCalculator: "
	      << std::chrono::duration_cast<std::chrono::milliseconds>(time_dmap_end - time_dmap_start).count() << " ms" << std::endl;

    computePath(_cost_image, _path_map, _goal_pixel, _obstacle_path);

    _path = _obstacle_path;

    if (!_path.size()){
      if (_robot_pose_pixel == _goal_pixel){
	// Path is zero because robot is on the goal
	publishResult(GoalReached);
      }else{
	publishResult(Aborted);
      }
      cancelGoal();
    } else {
      bool goal_reached = computeControlToWaypoint();
      
      if (goal_reached){
	publishResult(GoalReached);
	cancelGoal();
      }else
	applyVelocities();
    }

    std::chrono::steady_clock::time_point time_end = std::chrono::steady_clock::now();
    int cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count();
    std::cerr << "Cycle " << cycle_ms << " ms" << std::endl << std::endl;
 
  }



  void Planner::computePath(FloatImage& cost_map, PathMap& path_map, Eigen::Vector2i& goal, Vector2iVector &path){
    std::chrono::steady_clock::time_point time_path_start = std::chrono::steady_clock::now();
    _path_calculator.setMaxCost(_max_cost-1);
    _path_calculator.setCostMap(cost_map);
    _path_calculator.setOutputPathMap(path_map);
    Vector2iVector goals;
    goals.push_back(goal);
    _path_calculator.goals() = goals;
    _path_calculator.compute();
    std::chrono::steady_clock::time_point time_path_end = std::chrono::steady_clock::now();
    path.clear();
    // Filling path
    PathMapCell* current=&path_map(_robot_pose_pixel.x(), _robot_pose_pixel.y());
    while (current && current->parent && current->parent!=current) {
      PathMapCell* parent=current->parent;
      path.push_back(Eigen::Vector2i(current->r, current->c));
      current = current->parent;
    }
  }


  bool Planner::computeControlToWaypoint(){
    
    // Next waypoint naive computation.
    float nextwp_distance = 1.0; //meters
    int num_cells = nextwp_distance * _map_inverse_resolution;

    Eigen::Vector2i nextwp;
    bool isLastWp = false;
    if (_path.size() > num_cells)
      nextwp = _path[num_cells];
    else{
      nextwp = _path[_path.size()-1];
      isLastWp = true;
    }


    bool goal_reached = false;
    if (isLastWp){
      if (_have_goal_with_angle){
	// Giving (x, y, theta) to controller to arrive with the given robot orientation
	goal_reached = _motion_controller.computeVelocities(_robot_pose_image, _goal_image, _velocities);
      }
      else{
	// Giving (x, y) to controller. We do not care about the final robot orientation
	Eigen::Vector2f goal_image_xy(_goal_image.x(), _goal_image.y());
	goal_reached = _motion_controller.computeVelocities(_robot_pose_image, goal_image_xy, _velocities);
      }
    }
    else {
      // Giving (x, y) to controller since we still have a long way till reaching the goal
      Eigen::Vector2f nextwp_image_xy = grid2world(nextwp);
      goal_reached = _motion_controller.computeVelocities(_robot_pose_image, nextwp_image_xy, _velocities);
    }
  
    return goal_reached;
    
  }

  void Planner::startSubscribers(){
    std::cerr << "Starting subscribers." << std::endl;
    
    subscribeLaserWithPose();
    subscribeGoal();
    subscribeMap();
    subscribeCancel();
    subscribeReset();
  }

  void Planner::startPublishers(){
    std::cerr << "Starting publishers." << std::endl;

    startCmdVelPublisher();
    startPathPublisher();
    startResultPublisher();
  }

  void Planner::init(){
    std::cerr << "Starting Planner." << std::endl;
    
    startSubscribers();
    startPublishers();

    if (_use_gui)
      initGUI();    
    
  }

  void Planner::run(){
    if (_have_goal)
      plannerStep();

    if (_use_gui)
      handleGUI();

    if (_path.size())
      publishPath();
  }


  /*
  void Planner::manageEvent(Event event){

    State next_state;
    if (event == MapReceived){
      // Leave current goal active???
      // or wait for a new goal, in that case we should cancel current goal

      next_state = WaitingForGoal;
    }

    if (event == GoalReceived){
      // should cancel previous goal
      next_state = GoalAccepted;
    }
    
    if (_state == GoalAccepted && event == PathFound){
    
    next_state = ExecutingPath;

    }
    
    if (event == GoalReached){
    publishResult(GoalReached);
    next_state = WaitingForGoal;
    }

    if (event == Cancel){
    if (_state != WaitingForMap && _state != WaitingForGoal )
    cancelGoal();
    }





  void Planner::run(){
    if (_state == GoalAccepted || _state == ExecutingPath)
      plannerStep();

    if (_use_gui)
      handleGUI();

    if (_path.size())
      publishPath();
  }




*/



  
}
